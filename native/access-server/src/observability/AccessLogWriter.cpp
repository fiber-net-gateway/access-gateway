#include "AccessLogWriter.h"

#include "AccessLogLine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kCurrentLogFile = "access.log";
constexpr std::string_view kLogFilePrefix = "access-";
constexpr std::string_view kLogFileSuffix = ".log";

std::int64_t default_clock_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

std::string date_text(std::int64_t epoch_ms, int utc_offset_minutes) noexcept {
    // Reuse the line formatter's civil-date math: midnight of the shifted day
    // renders as "yyyy-MM-dd 00:00:00.000"; the first ten bytes are the date.
    std::array<char, 32> buffer{};
    const std::size_t length = format_access_log_timestamp(buffer.data(), epoch_ms, utc_offset_minutes);
    return std::string(buffer.data(), std::min<std::size_t>(length, 10));
}

int day_of_month(std::int64_t epoch_ms, int utc_offset_minutes) noexcept {
    std::array<char, 32> buffer{};
    (void) format_access_log_timestamp(buffer.data(), epoch_ms, utc_offset_minutes);
    int day = 0;
    for (std::size_t index = 8; index < 10; ++index) {
        day = day * 10 + (buffer[index] - '0');
    }
    return day;
}

std::string archive_path(const std::string &directory, const std::string &base, int index) {
    std::string name = base;
    name.push_back('.');
    name.append(std::to_string(index));
    name.append(kLogFileSuffix);
    std::string path = directory;
    if (!path.empty() && !path.ends_with('/')) {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

bool path_exists(const std::string &path) noexcept {
    struct stat file_stat{};
    return ::stat(path.c_str(), &file_stat) == 0;
}

std::int64_t file_mtime(const std::string &path) noexcept {
    struct stat file_stat{};
    if (::stat(path.c_str(), &file_stat) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(file_stat.st_mtim.tv_sec);
}

} // namespace

int local_utc_offset_minutes() noexcept {
    static const int offset = []() noexcept {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
        if (::localtime_r(&now, &local) == nullptr) {
            return 0;
        }
        return static_cast<int>(local.tm_gmtoff) / 60;
    }();
    return offset;
}

std::string resolve_local_server_ip() noexcept {
    // Java NetUtil.findFirstNonLoopbackAddress: across up interfaces, the
    // lowest interface index wins; within an interface the last non-loopback
    // IPv4 address wins.
    ifaddrs *addresses = nullptr;
    if (::getifaddrs(&addresses) != 0) {
        return "127.0.0.1";
    }
    std::string selected;
    unsigned selected_index = 0;
    bool has_selection = false;
    for (const ifaddrs *entry = addresses; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto *sin = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        const std::uint32_t address = sin->sin_addr.s_addr;
        if ((ntohl(address) & 0xff000000U) == 0x7f000000U) {
            continue;
        }
        char text[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const unsigned index = ::if_nametoindex(entry->ifa_name);
        if (!has_selection || (index != 0 && index < selected_index)) {
            selected = text;
            selected_index = index;
            has_selection = true;
        } else if (has_selection && index == selected_index) {
            selected = text;
        }
    }
    ::freeifaddrs(addresses);
    return has_selection ? selected : std::string("127.0.0.1");
}

AccessLogWriter::AccessLogWriter(std::string directory, std::string gateway_name, std::string server_ip,
                                 AccessLogWriterOptions options) :
    directory_(std::move(directory)), gateway_name_(std::move(gateway_name)), server_ip_(std::move(server_ip)),
    options_(std::move(options)), utc_offset_minutes_(local_utc_offset_minutes()) {
    buffer_.reserve(72 << 10);
    writer_thread_ = std::thread([this]() noexcept { run(); });
}

AccessLogWriter::~AccessLogWriter() { close(); }

std::int64_t AccessLogWriter::now_ms() const noexcept { return options_.clock ? options_.clock() : default_clock_ms(); }

void AccessLogWriter::run() noexcept {
    try {
        write_loop();
    } catch (...) {
        // The Java writer logs and lets the thread die; keep the same
        // non-fatal behavior.
        std::fprintf(stderr, "access-server access log writer loop terminated\n");
    }
}

void AccessLogWriter::write_loop() {
    std::deque<std::string> batch;
    for (;;) {
        bool closing_and_drained = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty() && !closing_) {
                if (buffer_.empty()) {
                    wakeup_.wait(lock, [this]() { return !queue_.empty() || closing_; });
                } else {
                    // Wall-clock deadline: libc++'s steady-clock wait_until
                    // targets pthread_cond_clockwait (glibc 2.30), which the
                    // CentOS 7 deployment target does not provide.
                    const auto deadline = last_write_ + options_.flush_interval;
                    wakeup_.wait_until(lock, deadline, [this, &deadline]() {
                        return !queue_.empty() || closing_ || std::chrono::system_clock::now() >= deadline;
                    });
                }
            }
            const std::size_t limit = std::min<std::size_t>(queue_.size(), 1024);
            for (std::size_t index = 0; index < limit; ++index) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            // Once closing_ is observed with the queue drained under the lock,
            // submit() can no longer add items: flush the buffered tail and
            // exit below instead of waiting out the flush interval.
            closing_and_drained = closing_ && batch.empty();
        }

        for (std::string &line: batch) {
            buffer_.append(line);
            if (buffer_.size() >= options_.flush_threshold) {
                flush_buffer();
            }
        }
        batch.clear();
        if (closing_and_drained) {
            if (!buffer_.empty()) {
                flush_buffer();
            }
            break;
        }
        if (!buffer_.empty()) {
            const auto now = std::chrono::system_clock::now();
            if (now - last_write_ >= options_.flush_interval) {
                flush_buffer();
            }
        }
    }
    if (!buffer_.empty()) {
        flush_buffer();
    }
    if (file_descriptor_ >= 0) {
        (void) ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
}

void AccessLogWriter::submit(std::string line) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || queue_.size() >= options_.queue_capacity) {
        dropped_lines_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.push_back(std::move(line));
    wakeup_.notify_one();
}

void AccessLogWriter::close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    wakeup_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

void AccessLogWriter::flush_buffer() noexcept {
    if (buffer_.empty()) {
        return;
    }
    last_write_ = std::chrono::system_clock::now();
    if (file_descriptor_ < 0 && !open_log_file()) {
        buffer_.clear();
        return;
    }
    const char *data = buffer_.data();
    std::size_t remaining = buffer_.size();
    bool failed = false;
    while (remaining > 0) {
        const ssize_t written = ::write(file_descriptor_, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = true;
            break;
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    if (failed) {
        std::fprintf(stderr, "access-server access log write failed: errno=%d\n", errno);
        current_file_size_ = 0;
        (void) ::close(file_descriptor_);
        file_descriptor_ = -1;
        (void) open_log_file();
        buffer_.clear();
        return;
    }
    current_file_size_ += buffer_.size();
    // Every access-log line ends with '\n', so newline counting reconciles
    // written lines against submit/drop accounting.
    written_lines_.fetch_add(static_cast<std::uint64_t>(std::count(buffer_.begin(), buffer_.end(), '\n')),
                             std::memory_order_relaxed);
    buffer_.clear();
    if (current_file_size_ > options_.max_file_size) {
        const std::int64_t now = now_ms();
        rotate_file(now);
        cleanup_old_files(now);
    }
}

bool AccessLogWriter::open_log_file() noexcept {
    if (file_descriptor_ >= 0) {
        (void) ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
    std::string path = directory_;
    if (!path.empty() && !path.ends_with('/')) {
        path.push_back('/');
    }
    path.append(kCurrentLogFile);

    struct stat file_stat{};
    if (::stat(path.c_str(), &file_stat) == 0) {
        current_file_size_ = static_cast<std::uint64_t>(file_stat.st_size);
        if (current_file_size_ > options_.max_file_size) {
            rotate_file(now_ms());
            current_file_size_ = 0;
        }
    } else {
        current_file_size_ = 0;
    }

    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        std::fprintf(stderr, "access-server failed to open access log %s: errno=%d\n", path.c_str(), errno);
        return false;
    }
    file_descriptor_ = descriptor;
    cleanup_old_files(now_ms());
    return true;
}

void AccessLogWriter::rotate_file(std::int64_t now_ms_value) noexcept {
    if (file_descriptor_ >= 0) {
        (void) ::close(file_descriptor_);
        file_descriptor_ = -1;
    }
    const std::string date = date_text(now_ms_value, utc_offset_minutes_);
    const std::string base = std::string(kLogFilePrefix) + date;
    const int today = day_of_month(now_ms_value, utc_offset_minutes_);

    std::string target;
    if (last_rotation_day_ != today) {
        archive_index_ = 0;
        target = archive_path(directory_, base, archive_index_);
        if (last_rotation_day_ == -1) {
            while (path_exists(target) && archive_index_ < 1000) {
                ++archive_index_;
                target = archive_path(directory_, base, archive_index_);
            }
        }
    } else {
        // archive_index_ names the archive the previous rotation produced;
        // the next same-day archive takes the following index.
        ++archive_index_;
        target = archive_path(directory_, base, archive_index_);
    }
    last_rotation_day_ = today;

    std::string current = directory_;
    if (!current.empty() && !current.ends_with('/')) {
        current.push_back('/');
    }
    current.append(kCurrentLogFile);
    if (::rename(current.c_str(), target.c_str()) != 0) {
        std::fprintf(stderr, "access-server failed to rotate access log to %s: errno=%d\n", target.c_str(), errno);
    }
}

void AccessLogWriter::cleanup_old_files(std::int64_t now_ms_value) noexcept {
    struct Entry {
        std::string path;
        std::int64_t modified;
    };
    std::vector<Entry> entries;
    DIR *directory = ::opendir(directory_.c_str());
    if (directory == nullptr) {
        return;
    }
    for (;;) {
        errno = 0;
        const dirent *item = ::readdir(directory);
        if (item == nullptr) {
            break;
        }
        const std::string_view name(item->d_name);
        if (!name.starts_with(kLogFilePrefix) || !name.ends_with(kLogFileSuffix) || name == kCurrentLogFile) {
            continue;
        }
        std::string path = directory_;
        if (!path.empty() && !path.ends_with('/')) {
            path.push_back('/');
        }
        path.append(name);
        // Stat before moving: braced-init-list evaluates left to right, so a
        // {std::move(path), file_mtime(path)} pair would stat the emptied path.
        const std::int64_t modified = file_mtime(path);
        entries.push_back({std::move(path), modified});
    }
    ::closedir(directory);

    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right) { return left.modified < right.modified; });
    const std::int64_t expire =
            now_ms_value / 1000 - std::chrono::duration_cast<std::chrono::seconds>(options_.retention).count();
    const std::size_t excess =
            entries.size() > options_.max_history_files ? entries.size() - options_.max_history_files : 0;
    std::size_t index = 0;
    for (; index < excess; ++index) {
        (void) ::unlink(entries[index].path.c_str());
    }
    for (; index < entries.size(); ++index) {
        if (entries[index].modified < expire) {
            (void) ::unlink(entries[index].path.c_str());
        }
    }
}

namespace {

std::atomic<AccessLogWriter *> g_access_log_writer{nullptr};

} // namespace

void initialize_access_log_writer(std::string directory, std::string gateway_name,
                                  AccessLogWriterOptions options) noexcept {
    FIBER_ASSERT(g_access_log_writer.load(std::memory_order_acquire) == nullptr);
    try {
        auto writer = std::make_unique<AccessLogWriter>(std::move(directory), std::move(gateway_name),
                                                        resolve_local_server_ip(), std::move(options));
        g_access_log_writer.store(writer.release(), std::memory_order_release);
    } catch (...) {
        std::fprintf(stderr, "access-server failed to start the access log writer\n");
    }
}

AccessLogWriter *access_log_writer() noexcept { return g_access_log_writer.load(std::memory_order_acquire); }

void shutdown_access_log_writer() noexcept {
    AccessLogWriter *writer = g_access_log_writer.exchange(nullptr, std::memory_order_acq_rel);
    if (writer == nullptr) {
        return;
    }
    writer->close();
    delete writer;
}

} // namespace fiber::access_server
