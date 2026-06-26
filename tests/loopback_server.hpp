#ifndef MOOCODE_LOOPBACK_SERVER_HPP
#define MOOCODE_LOOPBACK_SERVER_HPP

// A throwaway single-connection-at-a-time HTTP/1.1 server bound to 127.0.0.1 on
// an ephemeral port, for exercising the real libcurl client without a network.
// Captures the last request and replies with a configured status/body, with an
// optional pre-reply delay to provoke client timeouts.

#include <netinet/in.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace moocode::test {

class LoopbackServer {
public:
    LoopbackServer() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(fd_, 1);
    }

    ~LoopbackServer() {
        stop();
        if (fd_ >= 0) ::close(fd_);
    }

    // Begin serving `count` requests in a background thread.
    void serve(int status, std::string body, int count = 1,
               std::chrono::milliseconds delay = std::chrono::milliseconds{0}) {
        status_ = status;
        body_ = std::move(body);
        thread_ = std::thread([this, count, delay] {
            for (int i = 0; i < count && !stop_; ++i) handle_one(status_, body_, delay);
        });
    }

    // Serve a fixed sequence of (status, body) replies, one per accepted
    // connection, in order. Lets a test drive the retry/recovery path, e.g. a
    // 502 followed by a 200.
    void serve_each(std::vector<std::pair<int, std::string>> replies,
                    std::chrono::milliseconds delay = std::chrono::milliseconds{0}) {
        replies_ = std::move(replies);
        thread_ = std::thread([this, delay] {
            for (size_t i = 0; i < replies_.size() && !stop_; ++i)
                handle_one(replies_[i].first, replies_[i].second, delay);
        });
    }

    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    std::string url(const std::string& path = "/") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    int port() const { return port_; }
    std::string last_request() const { return last_request_; }
    std::string last_body() const { return last_body_; }

private:
    void handle_one(int status, const std::string& body,
                    std::chrono::milliseconds delay) {
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) return;
        std::string req;
        char buf[4096];
        // Read headers, then body per Content-Length.
        size_t content_len = 0;
        bool have_headers = false;
        for (;;) {
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (!have_headers) {
                auto hdr_end = req.find("\r\n\r\n");
                if (hdr_end != std::string::npos) {
                    have_headers = true;
                    content_len = parse_content_length(req);
                    size_t body_have = req.size() - (hdr_end + 4);
                    if (body_have >= content_len) break;
                }
            } else {
                auto hdr_end = req.find("\r\n\r\n");
                if (req.size() - (hdr_end + 4) >= content_len) break;
            }
        }
        last_request_ = req;
        if (auto p = req.find("\r\n\r\n"); p != std::string::npos)
            last_body_ = req.substr(p + 4);

        if (delay.count() > 0) std::this_thread::sleep_for(delay);

        std::string resp = "HTTP/1.1 " + std::to_string(status) + " X\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        resp += "Connection: close\r\n\r\n";
        resp += body;
        ::send(c, resp.data(), resp.size(), 0);
        ::close(c);
    }

    static size_t parse_content_length(const std::string& req) {
        // Case-insensitive search for the header.
        std::string lower = req;
        for (auto& ch : lower) ch = static_cast<char>(::tolower(ch));
        auto p = lower.find("content-length:");
        if (p == std::string::npos) return 0;
        p += std::strlen("content-length:");
        return static_cast<size_t>(std::strtoul(req.c_str() + p, nullptr, 10));
    }

    int fd_ = -1;
    int port_ = 0;
    int status_ = 200;
    std::string body_;
    std::vector<std::pair<int, std::string>> replies_;  // for serve_each
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::string last_request_;
    std::string last_body_;
};

}  // namespace moocode::test

#endif  // MOOCODE_LOOPBACK_SERVER_HPP
