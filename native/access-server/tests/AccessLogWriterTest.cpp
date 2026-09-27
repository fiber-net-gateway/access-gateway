#include "observability/AccessLogLine.h"
#include "observability/AccessLogWriter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fiber::access_server {
namespace {

// Fixed clock at 2025-10-09 16:53:20.123 local (+480), matching the golden
// AccessLogLineTest timestamps.
constexpr std::int64_t kFixedClockMs = 1760000000123;
constexpr std::int64_t kFixedClockSeconds = kFixedClockMs / 1000;

std::string read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_file(const std::string &path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void set_mtime(const std::string &path, std::int64_t seconds) {
    timespec times[2]{};
    times[0].tv_sec = seconds;
    times[1].tv_sec = seconds;
    ASSERT_EQ(::utimensat(AT_FDCWD, path.c_str(), times, 0), 0) << path;
}

std::vector<std::string> list_files(const std::string &directory) {
    std::vector<std::string> names;
    DIR *handle = ::opendir(directory.c_str());
    if (handle == nullptr) {
        return names;
    }
    for (;;) {
        const dirent *item = ::readdir(handle);
        if (item == nullptr) {
            break;
        }
        const std::string_view name(item->d_name);
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
    }
    ::closedir(handle);
    std::sort(names.begin(), names.end());
    return names;
}

bool list_has_file(const std::string &directory, std::string_view name) {
    const std::vector<std::string> names = list_files(directory);
    return std::find(names.begin(), names.end(), name) != names.end();
}

class TempLogDirectory {
public:
    TempLogDirectory() {
        std::string pattern = "/tmp/access-log-writer-test-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char *created = ::mkdtemp(buffer.data());
        if (created == nullptr) {
            ADD_FAILURE() << "failed to create a temporary directory";
            return;
        }
        path_ = created;
    }

    ~TempLogDirectory() {
        if (!path_.empty()) {
            for (const std::string &name: list_files(path_)) {
                (void) ::unlink((path_ + "/" + name).c_str());
            }
            (void) ::rmdir(path_.c_str());
        }
    }

    TempLogDirectory(const TempLogDirectory &) = delete;
    TempLogDirectory &operator=(const TempLogDirectory &) = delete;

    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
};

AccessLogWriterOptions writer_options() {
    AccessLogWriterOptions options;
    options.clock = []() noexcept { return kFixedClockMs; };
    return options;
}

std::string encode_line(std::uint64_t sequence) {
    AccessLogRecord record{};
    record.timestamp_ms = kFixedClockMs;
    record.gateway_name = "access-server-test";
    record.trace_id = "3f2a1c9d0b7e4f5a";
    record.http_method = "GET";
    record.received_path = "/benchmark";
    record.server_ip = "127.0.0.1";
    record.app_name = "ploto-orders";
    record.gw_cluster = "default";
    record.ctx_cluster = "-";
    record.ultimate_cluster = "default";
    record.backend_ip = "10.0.0.1";
    record.forward_uri = "/benchmark";
    record.http_code = 200;
    record.connect_nano = -static_cast<std::int64_t>(sequence);
    record.response_millis = static_cast<std::int64_t>(sequence);
    std::string line;
    encode_access_log_line(record, 480, line);
    return line;
}

TEST(AccessLogWriterTest, WritesSubmittedLinesAndDrainsOnClose) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", writer_options());
    for (std::uint64_t sequence = 0; sequence < 100; ++sequence) {
        writer.submit(encode_line(sequence));
    }
    writer.close();

    const std::string content = read_file(directory.path() + "/access.log");
    EXPECT_EQ(writer.written_lines(), 100u);
    EXPECT_EQ(writer.dropped_lines(), 0u);
    // Lines arrive in submit order and every line is complete.
    std::size_t expected_sequence = 0;
    std::size_t position = 0;
    while (position < content.size()) {
        const std::size_t newline = content.find('\n', position);
        ASSERT_NE(newline, std::string::npos);
        const std::string expected = encode_line(expected_sequence);
        EXPECT_EQ(content.substr(position, newline - position),
                  std::string_view(expected).substr(0, expected.size() - 1))
                << expected_sequence;
        ++expected_sequence;
        position = newline + 1;
    }
    EXPECT_EQ(expected_sequence, 100u);
}

TEST(AccessLogWriterTest, DropsAndNeverBlocksWhenQueueIsFull) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    AccessLogWriterOptions options = writer_options();
    options.queue_capacity = 8;
    // Keep flushed batches small so the writer thread keeps draining.
    options.flush_threshold = 256;
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", options);
    constexpr std::uint64_t kSubmitted = 5000;
    for (std::uint64_t sequence = 0; sequence < kSubmitted; ++sequence) {
        writer.submit(encode_line(sequence));
    }
    writer.close();
    // Every submitted line was either written or dropped; submit never blocks.
    EXPECT_EQ(writer.written_lines() + writer.dropped_lines(), kSubmitted);
}

TEST(AccessLogWriterTest, CloseFlushesBufferedTailWithoutWaitingOutFlushInterval) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    AccessLogWriterOptions options = writer_options();
    // The tail line stays below the size threshold, so only close() can flush
    // it. With a long interval, a writer that waits out the deadline instead
    // of draining on close would stall past the asserted budget.
    options.flush_interval = std::chrono::milliseconds(5000);
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", options);
    writer.submit(encode_line(0));
    // Wait for the first line to reach the file so the last-flush clock is
    // recent when the tail line is submitted.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (read_file(directory.path() + "/access.log").empty()) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "first line never flushed";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    writer.submit(encode_line(1));
    const auto close_started = std::chrono::steady_clock::now();
    writer.close();
    EXPECT_LT(std::chrono::steady_clock::now() - close_started, std::chrono::milliseconds(2000));
    EXPECT_EQ(read_file(directory.path() + "/access.log"), encode_line(0) + encode_line(1));
}

TEST(AccessLogWriterTest, RotatesOversizedFileOnOpenWithJavaArchiveNaming) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());
    write_file(directory.path() + "/access.log", std::string(4096, 'x'));

    AccessLogWriterOptions options = writer_options();
    options.max_file_size = 1024;
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", options);
    writer.submit(encode_line(0));
    writer.close();

    // The oversized current file was archived under the Java name for the
    // fixed clock's local date, and writing started a fresh access.log.
    EXPECT_EQ(read_file(directory.path() + "/access-2025-10-09.0.log"), std::string(4096, 'x'));
    EXPECT_EQ(read_file(directory.path() + "/access.log"), encode_line(0));
}

TEST(AccessLogWriterTest, RotatesWhenFlushedSizeExceedsLimitAndIncrementsIndex) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());

    AccessLogWriterOptions options = writer_options();
    options.max_file_size = 1024;
    options.flush_threshold = 256;
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", options);
    // ~180-byte lines against a 1024-byte limit force several rotations. The
    // exact archive count depends on writer-thread batching, so the test checks
    // the invariants: contiguous Java-style indices, no data loss, and submit
    // order preserved across the archive sequence followed by access.log.
    constexpr std::uint64_t kLines = 24;
    for (std::uint64_t sequence = 0; sequence < kLines; ++sequence) {
        writer.submit(encode_line(sequence));
    }
    writer.close();
    ASSERT_EQ(writer.written_lines(), kLines);
    ASSERT_EQ(writer.dropped_lines(), 0u);

    const std::vector<std::string> names = list_files(directory.path());
    std::vector<std::string> archives;
    bool has_current = false;
    for (const std::string &name: names) {
        if (name.starts_with("access-2025-10-09.") && name.ends_with(".log")) {
            archives.push_back(name);
        } else if (name == "access.log") {
            has_current = true;
        } else {
            ADD_FAILURE() << "unexpected file " << name;
        }
    }
    ASSERT_FALSE(archives.empty());
    std::string concatenated;
    std::size_t total = 0;
    for (std::uint64_t sequence = 0; sequence < kLines; ++sequence) {
        concatenated += encode_line(sequence);
        total += encode_line(sequence).size();
    }
    std::string ordered;
    for (const std::string &name: archives) {
        ordered += read_file(directory.path() + "/" + name);
    }
    if (has_current) {
        ordered += read_file(directory.path() + "/access.log");
    }
    EXPECT_EQ(ordered, concatenated);
    // Archive indices from the fixed-clock day are contiguous starting at 0.
    for (std::size_t index = 0; index < archives.size(); ++index) {
        EXPECT_EQ(archives[index], "access-2025-10-09." + std::to_string(index) + ".log");
    }
}

TEST(AccessLogWriterTest, DeletesExpiredAndExcessArchives) {
    TempLogDirectory directory;
    ASSERT_FALSE(directory.path().empty());

    const std::int64_t retention_seconds = 30LL * 24 * 3600;
    const std::string a = directory.path() + "/access-2025-09-01.0.log";
    const std::string b = directory.path() + "/access-2025-09-02.0.log";
    const std::string c = directory.path() + "/access-2025-10-08.0.log";
    const std::string d = directory.path() + "/access-2025-10-08.1.log";
    const std::string e = directory.path() + "/access-2025-10-08.2.log";
    write_file(a, "a");
    write_file(b, "b");
    write_file(c, "c");
    write_file(d, "d");
    write_file(e, "e");
    set_mtime(a, kFixedClockSeconds - retention_seconds - 3600);
    set_mtime(b, kFixedClockSeconds - retention_seconds - 1800);
    set_mtime(c, kFixedClockSeconds - 3600);
    set_mtime(d, kFixedClockSeconds - retention_seconds - 60);
    set_mtime(e, kFixedClockSeconds - 60);

    AccessLogWriterOptions options = writer_options();
    options.max_history_files = 3;
    AccessLogWriter writer(directory.path(), "access-server-test", "127.0.0.1", options);
    writer.submit(encode_line(0));
    writer.close();

    // Sorted by mtime: a, b, d (expired but newer), c, e. The count cap of 3
    // deletes a and b first; the retention pass then deletes d, keeping c and e.
    EXPECT_FALSE(list_has_file(directory.path(), "access-2025-09-01.0.log"));
    EXPECT_FALSE(list_has_file(directory.path(), "access-2025-09-02.0.log"));
    EXPECT_FALSE(list_has_file(directory.path(), "access-2025-10-08.1.log"));
    EXPECT_TRUE(list_has_file(directory.path(), "access-2025-10-08.0.log"));
    EXPECT_TRUE(list_has_file(directory.path(), "access-2025-10-08.2.log"));
    EXPECT_TRUE(list_has_file(directory.path(), "access.log"));
}

TEST(AccessLogWriterTest, ResolvesNonLoopbackServerIp) {
    // The NetUtil port returns a dotted IPv4 (or the 127.0.0.1 fallback) on
    // every machine this test can run on; both satisfy the log format.
    const std::string address = resolve_local_server_ip();
    EXPECT_FALSE(address.empty());
    EXPECT_EQ(address.find_first_not_of("0123456789."), std::string::npos);
}

} // namespace
} // namespace fiber::access_server
