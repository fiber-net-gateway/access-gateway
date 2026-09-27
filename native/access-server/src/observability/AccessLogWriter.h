#ifndef FIBER_ACCESS_SERVER_ACCESS_LOG_WRITER_H
#define FIBER_ACCESS_SERVER_ACCESS_LOG_WRITER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

struct AccessLogWriterOptions {
    // Mirrors AsyncAccessLogWriter constants.
    std::size_t queue_capacity = 20000;
    std::uint64_t max_file_size = 100ULL << 20;
    std::size_t max_history_files = 300;
    std::chrono::seconds retention = std::chrono::seconds(30 * 24 * 3600);
    std::size_t flush_threshold = 63 << 10;
    std::chrono::milliseconds flush_interval{1000};
    // Injectable clock for tests; epoch milliseconds.
    std::function<std::int64_t()> clock;
};

// Async file writer for the Java-compatible access log: one dedicated thread,
// bounded drop-on-full queue, size-based rotation to access-YYYY-MM-DD.<N>.log
// archives, and history cleanup. Submitters are request workers; the writer
// never blocks them on file I/O.
class AccessLogWriter final : public common::NonCopyable, public common::NonMovable {
public:
    AccessLogWriter(std::string directory, std::string gateway_name, std::string server_ip,
                    AccessLogWriterOptions options = {});
    ~AccessLogWriter();

    // Enqueues one pre-encoded line. Never blocks; drops (and counts) when the
    // queue is full so logging cannot stall request workers.
    void submit(std::string line) noexcept;

    // Stops accepting new lines, drains the queue and buffer, and joins the
    // writer thread.
    void close() noexcept;

    [[nodiscard]] std::string_view gateway_name() const noexcept { return gateway_name_; }
    [[nodiscard]] std::string_view server_ip() const noexcept { return server_ip_; }
    [[nodiscard]] int utc_offset_minutes() const noexcept { return utc_offset_minutes_; }
    [[nodiscard]] std::uint64_t dropped_lines() const noexcept {
        return dropped_lines_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t written_lines() const noexcept {
        return written_lines_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept;
    void write_loop();
    bool open_log_file() noexcept;
    void rotate_file(std::int64_t now_ms) noexcept;
    void cleanup_old_files(std::int64_t now_ms) noexcept;
    void flush_buffer() noexcept;
    [[nodiscard]] std::int64_t now_ms() const noexcept;

    const std::string directory_;
    const std::string gateway_name_;
    const std::string server_ip_;
    const AccessLogWriterOptions options_;
    const int utc_offset_minutes_;

    std::mutex mutex_;
    std::condition_variable wakeup_;
    std::deque<std::string> queue_;
    bool closing_ = false;

    std::thread writer_thread_;
    int file_descriptor_ = -1;
    std::string buffer_;
    std::uint64_t current_file_size_ = 0;
    int archive_index_ = 0;
    int last_rotation_day_ = -1;
    // Wall clock so the timed wait stays on pthread_cond_timedwait (glibc 2.17
    // compatible); a wall-clock step can only shift a flush deadline.
    std::chrono::system_clock::time_point last_write_{};

    std::atomic_uint64_t dropped_lines_{0};
    std::atomic_uint64_t written_lines_{0};
};

// Process-wide writer. initialize_access_log_writer installs it; until then
// (or after shutdown) access_log_writer() returns null and callers skip
// logging. Initialization resolves the server IP once and captures the local
// UTC offset the way the Java gateway does (fixed for the process lifetime).
void initialize_access_log_writer(std::string directory, std::string gateway_name,
                                  AccessLogWriterOptions options = {}) noexcept;
[[nodiscard]] AccessLogWriter *access_log_writer() noexcept;
void shutdown_access_log_writer() noexcept;

// Localhost IPv4 chosen by the Java NetUtil rules: the lowest-index up
// interface's last non-loopback IPv4 address, or 127.0.0.1 when none exists.
[[nodiscard]] std::string resolve_local_server_ip() noexcept;

// Current local UTC offset in minutes (fixed at first call).
[[nodiscard]] int local_utc_offset_minutes() noexcept;

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_LOG_WRITER_H
