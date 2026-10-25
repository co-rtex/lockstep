#include "enginex/client.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <stdexcept>

namespace enginex::net {

using Clock = std::chrono::steady_clock;

Client::Client(Socket socket) : socket_(std::move(socket)) {}

Client Client::connect(const std::string& host, std::uint16_t port,
                       std::chrono::milliseconds timeout) {
    Socket s = connect_tcp(host, port, timeout);
    configure_stream(s.fd(), std::chrono::milliseconds(0), std::chrono::seconds(10));
    return Client(std::move(s));
}

bool Client::send(codec::Json message) {
    if (!message.contains("protocol_version")) {
        codec::Json withv;
        withv["protocol_version"] = kProtocolVersion;
        for (auto it = message.begin(); it != message.end(); ++it) {
            withv[it.key()] = it.value();
        }
        message = std::move(withv);
    }
    return send_raw(encode_frame(message.dump(), kMaxClientFrame));
}

bool Client::send_raw(std::string_view bytes) {
    if (closed_ || write_failed_ || !socket_.valid() || send_all(socket_.fd(), bytes) != 0) {
        write_failed_ = true;
        return false;
    }
    return true;
}

std::optional<codec::Json> Client::receive(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::string payload;
    char buf[64 * 1024];
    while (true) {
        const auto status = decoder_.next(payload);
        if (status == FrameDecoder::Status::Frame) {
            codec::Json j = codec::Json::parse(payload, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded()) {
                throw std::runtime_error("server sent malformed JSON");
            }
            return j;
        }
        if (status == FrameDecoder::Status::Error) {
            throw std::runtime_error("server sent an invalid frame: " + decoder_.error());
        }
        if (closed_ || !socket_.valid()) {
            return std::nullopt;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) {
            return std::nullopt;
        }
        pollfd pfd{socket_.fd(), POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(left.count()));
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            continue;  // timeout re-checked above
        }
        const ssize_t n = ::recv(socket_.fd(), buf, sizeof buf, 0);
        if (n > 0) {
            decoder_.feed(buf, static_cast<std::size_t>(n));
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
            closed_ = true;
        }
    }
}

std::optional<codec::Json> Client::request(codec::Json message, std::chrono::milliseconds timeout) {
    const std::string rid = message.value("request_id", std::string());
    if (!send(std::move(message))) {
        return std::nullopt;
    }
    const auto deadline = Clock::now() + timeout;
    while (true) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) {
            return std::nullopt;
        }
        auto msg = receive(left);
        if (!msg) {
            return std::nullopt;
        }
        if (msg->value("type", "") == "book_update") {
            continue;
        }
        if (msg->value("request_id", std::string()) == rid) {
            return msg;
        }
    }
}

void Client::shutdown_write() {
    ::shutdown(socket_.fd(), SHUT_WR);
}

void Client::close() {
    socket_.reset();
    closed_ = true;
}

}  // namespace enginex::net
