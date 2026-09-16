#pragma once

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cosmo::test {

class LoopbackHttpServer final {
public:
    struct Response {
        int status;
        std::string body;
    };

    LoopbackHttpServer() = default;

    ~LoopbackHttpServer() {
        CloseListener();
    }

    LoopbackHttpServer(const LoopbackHttpServer&)            = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    bool Start() {
        CloseListener();
        listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            return false;
        }

        int reuse = 1;
        if (setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            CloseListener();
            return false;
        }

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            CloseListener();
            return false;
        }

        socklen_t address_size = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
            CloseListener();
            return false;
        }
        port_ = ntohs(address.sin_port);
        return true;
    }

    std::uint16_t Port() const {
        return port_;
    }

    void StopServing() {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        if (listener_ >= 0) {
            shutdown(listener_, SHUT_RDWR);
        }
    }

    bool ServeOnce(std::size_t response_body_size, char response_fill,
                   std::string* captured_request = nullptr) {
        const bool success =
            ServeResponse(response_body_size, response_fill, nullptr, 200, captured_request, {});
        CloseListener();
        return success;
    }

    bool ServeResponses(const std::vector<Response>& responses,
                        std::vector<std::string>* captured_requests             = nullptr,
                        const std::function<bool(std::size_t)>& before_response = {}) {
        bool success = true;
        for (std::size_t index = 0; success && index < responses.size(); ++index) {
            const auto& response = responses[index];
            std::string request;
            success = ServeResponse(response.body.size(), '\0', &response.body, response.status, &request,
                                    [&]() { return !before_response || before_response(index); });
            if (captured_requests != nullptr && !request.empty()) {
                captured_requests->push_back(std::move(request));
            }
        }
        CloseListener();
        return success;
    }

private:
    bool ServeResponse(std::size_t response_body_size, char response_fill, const std::string* response_body,
                       int status, std::string* captured_request,
                       const std::function<bool()>& before_response) {
        if (listener_ < 0) {
            return false;
        }

        pollfd descriptor{listener_, POLLIN, 0};
        if (poll(&descriptor, 1, static_cast<int>(std::chrono::seconds(5).count() * 1000)) != 1 ||
            (descriptor.revents & POLLIN) == 0) {
            CloseListener();
            return false;
        }

        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            return false;
        }

        timeval receive_timeout{};
        receive_timeout.tv_sec = 5;
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) != 0 ||
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &receive_timeout, sizeof(receive_timeout)) != 0) {
            close(client);
            return false;
        }

        std::string request;
        bool success = ReadRequest(client, request);
        if (success && captured_request != nullptr) {
            *captured_request = request;
        }
        success = success && (!before_response || before_response());

        const auto response_header = "HTTP/1.1 " + std::to_string(status) +
                                     " Test\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                                     std::to_string(response_body_size) + "\r\nConnection: close\r\n\r\n";
        success = success && SendAll(client, response_header.data(), response_header.size());

        std::array<char, 64 * 1024> response_buffer{};
        response_buffer.fill(response_fill);
        std::size_t remaining = response_body_size;
        while (success && remaining > 0) {
            const auto bytes = std::min(remaining, response_buffer.size());
            const auto* data = response_body == nullptr
                                   ? response_buffer.data()
                                   : response_body->data() + response_body_size - remaining;
            success          = SendAll(client, data, bytes);
            remaining -= bytes;
        }

        shutdown(client, SHUT_RDWR);
        close(client);
        return success;
    }

    static bool SendAll(int socket_fd, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const auto result = send(socket_fd, data + sent, size - sent, MSG_NOSIGNAL);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(result);
        }
        return true;
    }

    static bool ParseContentLength(const std::string& request, std::size_t header_end,
                                   std::size_t& content_length) {
        std::string lower_headers = request.substr(0, header_end + 2);
        std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

        constexpr char kHeaderName[] = "\r\ncontent-length:";
        const auto header_position   = lower_headers.find(kHeaderName);
        if (header_position == std::string::npos) {
            content_length = 0;
            return true;
        }

        auto value_begin = header_position + sizeof(kHeaderName) - 1;
        while (value_begin < header_end &&
               (lower_headers[value_begin] == ' ' || lower_headers[value_begin] == '\t')) {
            ++value_begin;
        }
        const auto value_end = lower_headers.find("\r\n", value_begin);
        if (value_end == std::string::npos) {
            return false;
        }

        std::size_t parsed_length = 0;
        const auto parse_result   = std::from_chars(lower_headers.data() + value_begin,
                                                    lower_headers.data() + value_end, parsed_length);
        if (parse_result.ec != std::errc{} || parse_result.ptr != lower_headers.data() + value_end) {
            return false;
        }
        content_length = parsed_length;
        return true;
    }

    static bool ReadRequest(int socket_fd, std::string& request) {
        constexpr std::size_t kMaxHeaderSize = 64 * 1024;
        constexpr std::size_t kMaxBodySize   = 4 * 1024 * 1024;
        std::array<char, 4096> buffer{};
        while (true) {
            const auto header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::size_t content_length = 0;
                if (!ParseContentLength(request, header_end, content_length) ||
                    content_length > kMaxBodySize) {
                    return false;
                }
                const auto expected_size = header_end + 4 + content_length;
                if (request.size() >= expected_size) {
                    request.resize(expected_size);
                    return true;
                }
            } else if (request.size() >= kMaxHeaderSize) {
                return false;
            }

            const auto received = recv(socket_fd, buffer.data(), buffer.size(), 0);
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received <= 0) {
                return false;
            }
            request.append(buffer.data(), static_cast<std::size_t>(received));
        }
    }

    void CloseListener() {
        std::lock_guard<std::mutex> lock(listener_mutex_);
        if (listener_ >= 0) {
            close(listener_);
            listener_ = -1;
        }
        port_ = 0;
    }

    int listener_{-1};
    std::mutex listener_mutex_;
    std::uint16_t port_{0};
};

}  // namespace cosmo::test
