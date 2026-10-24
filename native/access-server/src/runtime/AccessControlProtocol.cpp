#include "AccessControlProtocol.h"

#include <array>
#include <limits>
#include <utility>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/common/json/JsonParser.h>
#include <fiber/net/UnixAddress.h>

namespace fiber::access_server {
namespace {

struct TextField {
    std::string_view name;
    std::string AccessControlMessage::*member;
};

struct IntegerField {
    std::string_view name;
    std::int64_t AccessControlMessage::*member;
};

struct BoolField {
    std::string_view name;
    bool AccessControlMessage::*member;
};

constexpr std::array kTextFields{
        TextField{"requestId", &AccessControlMessage::request_id},
        TextField{"command", &AccessControlMessage::command},
        TextField{"upgradeId", &AccessControlMessage::upgrade_id},
        TextField{"state", &AccessControlMessage::state},
        TextField{"phase", &AccessControlMessage::phase},
        TextField{"registration", &AccessControlMessage::registration},
        TextField{"compatibility", &AccessControlMessage::compatibility},
        TextField{"error", &AccessControlMessage::error},
        TextField{"upgradeError", &AccessControlMessage::upgrade_error},
        TextField{"serverPath", &AccessControlMessage::server_path},
        TextField{"activeState", &AccessControlMessage::active_state},
        TextField{"activeRegistration", &AccessControlMessage::active_registration},
        TextField{"candidateState", &AccessControlMessage::candidate_state},
};

constexpr std::array kIntegerFields{
        IntegerField{"protocolVersion", &AccessControlMessage::protocol_version},
        IntegerField{"pid", &AccessControlMessage::pid},
        IntegerField{"activePid", &AccessControlMessage::active_pid},
        IntegerField{"drainingPid", &AccessControlMessage::draining_pid},
        IntegerField{"candidatePid", &AccessControlMessage::candidate_pid},
        IntegerField{"prewarmTimeoutMillis", &AccessControlMessage::prewarm_timeout_millis},
};

constexpr std::array kBoolFields{
        BoolField{"ready", &AccessControlMessage::ready},
        BoolField{"http3", &AccessControlMessage::http3},
        BoolField{"fixedPorts", &AccessControlMessage::fixed_ports},
        BoolField{"background", &AccessControlMessage::background},
};

constexpr std::size_t kTextCount = kTextFields.size();
constexpr std::size_t kIntegerCount = kIntegerFields.size();
constexpr std::size_t kFieldCount = kTextCount + kIntegerCount + kBoolFields.size();
static_assert(kFieldCount <= 64);
constexpr std::size_t kMaxTextField = 4096;

std::size_t field_index(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kTextCount; ++i) {
        if (kTextFields[i].name == name) {
            return i;
        }
    }
    for (std::size_t i = 0; i < kIntegerCount; ++i) {
        if (kIntegerFields[i].name == name) {
            return kTextCount + i;
        }
    }
    for (std::size_t i = 0; i < kBoolFields.size(); ++i) {
        if (kBoolFields[i].name == name) {
            return kTextCount + kIntegerCount + i;
        }
    }
    return kFieldCount;
}

class StringSink final : public json::OutputSink {
public:
    explicit StringSink(std::string &out) noexcept : out_(out) {}
    bool write(const char *data, std::size_t size) override {
        if (size > kAccessControlMaxFrame - out_.size()) {
            return false;
        }
        out_.append(data, size);
        return true;
    }

private:
    std::string &out_;
};

async::Task<common::IoResult<void>> read_exact(net::UnixStream &stream, char *out, std::size_t size,
                                               std::chrono::steady_clock::time_point deadline, std::size_t *consumed) {
    while (size != 0) {
        const auto now = event::EventLoop::current().now();
        if (now >= deadline) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        auto read = co_await stream.read(out, size, remaining);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        out += *read;
        size -= *read;
        if (consumed != nullptr) {
            *consumed += *read;
        }
    }
    co_return common::IoResult<void>{};
}

} // namespace

common::IoResult<AccessControlMessage> decode_access_control(std::string_view input) {
    if (input.empty() || input.size() > kAccessControlMaxFrame) {
        return std::unexpected(common::IoErr::Invalid);
    }
    json::JsonParser parser;
    if (!parser.feed(input.data(), input.size())) {
        return std::unexpected(common::IoErr::Invalid);
    }
    parser.finish();
    using Status = json::JsonParser::Status;
    using Kind = json::TokenKind;
    if (parser.next() != Status::Token || parser.current_token()->kind != Kind::StartObj) {
        return std::unexpected(common::IoErr::Invalid);
    }
    AccessControlMessage message;
    message.protocol_version = 0;
    std::uint64_t seen = 0;
    for (;;) {
        if (parser.next() != Status::Token) {
            return std::unexpected(common::IoErr::Invalid);
        }
        if (parser.current_token()->kind == Kind::EndObj) {
            break;
        }
        if (parser.current_token()->kind != Kind::Text || parser.current_token()->role != json::TokenRole::ObjectKey) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto index = field_index(parser.current_token()->view);
        if (index >= kFieldCount || (seen & (std::uint64_t{1} << index)) != 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        seen |= std::uint64_t{1} << index;
        if (parser.next() != Status::Token) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const auto &value = *parser.current_token();
        if (index < kTextCount) {
            if (value.kind != Kind::Text || value.view.size() > kMaxTextField ||
                value.view.find('\0') != std::string_view::npos) {
                return std::unexpected(common::IoErr::Invalid);
            }
            message.*kTextFields[index].member = value.view;
        } else if (index < kTextCount + kIntegerCount) {
            if (value.kind != Kind::Integer || value.inum < 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            message.*kIntegerFields[index - kTextCount].member = value.inum;
        } else {
            if (value.kind != Kind::Bool) {
                return std::unexpected(common::IoErr::Invalid);
            }
            message.*kBoolFields[index - kTextCount - kIntegerCount].member = value.bval;
        }
    }
    if (parser.next() != Status::Complete || message.protocol_version != kAccessControlProtocolVersion ||
        message.request_id.empty() || message.request_id.size() > 128 || message.upgrade_id.size() > 128 ||
        message.command.size() > 32) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return message;
}

common::IoResult<std::string> encode_access_control(const AccessControlMessage &message) {
    std::string output;
    StringSink sink(output);
    json::Generator generator(sink);
    using Result = json::Generator::Result;
    const auto text = [&generator](std::string_view value) {
        return generator.string(value.data(), value.size()) == Result::OK;
    };
    bool ok = generator.map_open() == Result::OK;
    for (const auto &field: kIntegerFields) {
        ok = ok && text(field.name) && generator.integer(message.*field.member) == Result::OK;
    }
    for (const auto &field: kBoolFields) {
        ok = ok && text(field.name) && generator.bool_value(message.*field.member) == Result::OK;
    }
    for (const auto &field: kTextFields) {
        ok = ok && text(field.name) && text(message.*field.member);
    }
    ok = ok && generator.map_close() == Result::OK;
    if (!ok) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return output;
}

async::Task<common::IoResult<AccessControlMessage>> read_access_control(net::UnixStream &stream, bool *frame_started) {
    const auto deadline = event::EventLoop::current().now() + kAccessControlTimeout;
    std::uint32_t wire_size = 0;
    std::size_t header_bytes = 0;
    auto header = co_await read_exact(stream, reinterpret_cast<char *>(&wire_size), sizeof(wire_size), deadline,
                                      &header_bytes);
    if (frame_started != nullptr) {
        *frame_started = header_bytes != 0;
    }
    if (!header) {
        co_return std::unexpected(header.error());
    }
    const auto size = ::ntohl(wire_size);
    if (size == 0 || size > kAccessControlMaxFrame) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    std::string input(size, '\0');
    auto body = co_await read_exact(stream, input.data(), input.size(), deadline, nullptr);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    co_return decode_access_control(input);
}

async::Task<common::IoResult<void>> write_access_control(net::UnixStream &stream, const AccessControlMessage &message) {
    auto encoded = encode_access_control(message);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    const auto size = ::htonl(static_cast<std::uint32_t>(encoded->size()));
    std::string frame(reinterpret_cast<const char *>(&size), sizeof(size));
    frame += *encoded;
    const auto deadline = event::EventLoop::current().now() + kAccessControlTimeout;
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const auto now = event::EventLoop::current().now();
        if (now >= deadline) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        auto result = co_await stream.write(frame.data() + offset, frame.size() - offset,
                                            std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        offset += *result;
    }
    co_return common::IoResult<void>{};
}

common::IoResult<void> validate_access_control_peer(int fd) noexcept {
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || size != sizeof(peer)) {
        return std::unexpected(common::IoErr::Permission);
    }
    const uid_t self = ::geteuid();
    if (peer.uid != self && peer.uid != 0 && self != 0) {
        return std::unexpected(common::IoErr::Permission);
    }
    return {};
}

async::Task<common::IoResult<AccessControlMessage>> request_access_control(event::EventLoop &loop, std::string path,
                                                                           AccessControlMessage request) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path) || path.front() != '/' ||
        path.find('\0') != std::string::npos) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto connected = co_await net::UnixStream::connect(loop, net::UnixAddress::filesystem(std::move(path)),
                                                       kAccessControlTimeout);
    if (!connected) {
        co_return std::unexpected(connected.error());
    }
    net::UnixStream stream(std::move(*connected));
    if (auto peer = validate_access_control_peer(stream.fd()); !peer) {
        co_return std::unexpected(peer.error());
    }
    co_return co_await exchange_access_control(stream, std::move(request));
}

async::Task<common::IoResult<AccessControlMessage>> exchange_access_control(net::UnixStream &stream,
                                                                            AccessControlMessage request) {
    auto sent = co_await write_access_control(stream, request);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    auto response = co_await read_access_control(stream);
    if (response && response->request_id != request.request_id) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return response;
}

} // namespace fiber::access_server
