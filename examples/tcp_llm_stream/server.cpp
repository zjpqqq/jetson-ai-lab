#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "llm_stream.hpp"
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <chrono>
#include <csignal>
#include <future>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

constexpr std::uint32_t MAX_BODY = 64 * 1024;

// 离开作用域时自动关闭 Socket，类似 Python 的 with。
class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {
        if (fd_ < 0) {
            throw std::system_error(
                errno, std::generic_category(), "socket/accept");
        }
    }

    ~Socket() {
        ::close(fd_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int get() const {
        return fd_;
    }

private:
    int fd_;
};

void check(int result, const char* operation) {
    if (result < 0) {
        throw std::system_error(
                errno, std::generic_category(), operation);
    }
}

// 收齐指定字节数。
// 尚未收到任何字节就遇到 EOF，返回 false。
// 收到一部分后断开，说明消息不完整，抛出异常。
bool recv_exact(int fd, void* buffer, std::size_t size) {
    auto* bytes = static_cast<char*>(buffer);
    std::size_t received = 0;

    while (received < size) {
        const ssize_t n =
                ::recv(fd, bytes + received, size - received, 0);

        if (n == 0) {
            if (received == 0) {
                return false;
            }
            throw std::runtime_error("对端在消息传输途中断开");
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;  // 被信号中断，重试本次接收
            }
            throw std::system_error(
                    errno, std::generic_category(), "recv");
        }

        received += static_cast<std::size_t>(n);
    }

    return true;
}

// send() 也可能只发送一部分，因此同样需要循环。
void send_all(int fd, const void* buffer, std::size_t size) {
    const auto* bytes = static_cast<const char*>(buffer);
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t n = ::send(
                fd, bytes + sent, size - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(
                    errno, std::generic_category(), "send");
        }

        if (n == 0) {
            throw std::runtime_error("发送未取得进展");
        }

        sent += static_cast<std::size_t>(n);
    }
}

void handle_client(int fd) {
    timeval timeout{};
    timeout.tv_sec = 30;

    check(::setsockopt(
              fd, SOL_SOCKET, SO_RCVTIMEO,
              &timeout, sizeof(timeout)),
          "SO_RCVTIMEO");

    check(::setsockopt(
              fd, SOL_SOCKET, SO_SNDTIMEO,
              &timeout, sizeof(timeout)),
          "SO_SNDTIMEO");

    // TCP 分包格式保持不变。
    auto send_message = [&](const json& message) {
        const std::string body = message.dump();

        if (body.empty() || body.size() > MAX_BODY) {
            throw std::runtime_error("响应长度超限");
        }

        const std::uint32_t size =
            htonl(static_cast<std::uint32_t>(body.size()));

        send_all(fd, &size, sizeof(size));
        send_all(fd, body.data(), body.size());
    };

    while (true) {
        std::uint32_t network_size = 0;

        if (!recv_exact(fd, &network_size, sizeof(network_size))) {
            std::cout << "客户端正常断开\n";
            return;
        }

        const std::uint32_t size = ntohl(network_size);

        if (size == 0 || size > MAX_BODY) {
            throw std::runtime_error("非法消息长度");
        }

        std::string body(size, '\0');

        if (!recv_exact(fd, body.data(), body.size())) {
            throw std::runtime_error("正文接收前连接断开");
        }

        const json request = json::parse(body);

        if (!request.is_object()) {
            throw std::runtime_error("请求必须是 JSON 对象");
        }

        const json request_id =
            request.value("request_id", json(nullptr));

        std::string finish_reason;

        try {
            const std::string text =
                request.at("text").get<std::string>();

            if (text.empty()) {
                throw std::runtime_error("text 不能为空");
            }

            finish_reason = llm::generate_stream(
                text,
                [&](const std::string& piece) {
                    send_message({
                        {"request_id", request_id},
                        {"type", "delta"},
                        {"text", piece}
                    });
                }
            );
        } catch (const std::exception& e) {
            // 如果此前已发出部分文本，这个 error 表示回答未完成。
            // 若 TCP 本身已断开，发送失败会交给外层处理并关闭连接。
            send_message({
                {"request_id", request_id},
                {"type", "error"},
                {"error", std::string(e.what()).substr(0, 500)}
            });

            continue;
        }

        // 只有 HTTP 成功且收到 [DONE] 后，才发送正常结束事件。
        send_message({
            {"request_id", request_id},
            {"type", "done"},
            {"finish_reason", finish_reason}
        });
    }
}

int main() {
    try {
        // 避免连接断开时 SIGPIPE 直接终止进程。
        std::signal(SIGPIPE, SIG_IGN);

        // 必须在线程启动前初始化 libcurl。
        llm::CurlRuntime curl_runtime;

        Socket server(::socket(AF_INET, SOCK_STREAM, 0));

        const int reuse = 1;
        check(::setsockopt(
                  server.get(), SOL_SOCKET, SO_REUSEADDR,
                  &reuse, sizeof(reuse)),
              "SO_REUSEADDR");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9003);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        check(::bind(
                  server.get(),
                  reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)),
              "bind");

        check(::listen(server.get(), 16), "listen");

        constexpr std::size_t MAX_CLIENTS = 4;

        // 保存工作线程对应的任务句柄。
        // 在 curl_runtime 之后声明，异常退出时会先等待任务结束，
        // 再清理 libcurl。
        std::vector<std::future<void>> jobs;
        jobs.reserve(MAX_CLIENTS);

        std::cout
            << "并发 TCP 服务监听 127.0.0.1:9002，最多 4 个连接\n";

        while (true) {
            const int fd =
                ::accept(server.get(), nullptr, nullptr);

            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }

                std::cerr << "accept 失败，errno="
                          << errno << '\n';
                continue;
            }

            // 清理已经结束的任务，不阻塞等待尚未完成的任务。
            for (auto it = jobs.begin(); it != jobs.end();) {
                if (it->wait_for(std::chrono::seconds(0))
                    == std::future_status::ready) {

                    try {
                        it->get();
                    } catch (const std::exception& e) {
                        std::cerr << "任务异常："
                                  << e.what() << '\n';
                    }

                    it = jobs.erase(it);
                } else {
                    ++it;
                }
            }

            if (jobs.size() >= MAX_CLIENTS) {
                std::cerr << "连接数量达到上限，关闭新连接\n";
                ::close(fd);
                continue;
            }

            try {
                jobs.emplace_back(std::async(
                    std::launch::async,
                    [fd]() {
                        // fd 按值传给工作线程。
                        // 连接的所有权交给这个 Socket 对象。
                        try {
                            Socket client(fd);

                            std::cout << "[连接 " << fd
                                      << "] 开始处理\n";

                            handle_client(client.get());

                            std::cout << "[连接 " << fd
                                      << "] 处理结束\n";
                        } catch (const std::exception& e) {
                            std::cerr << "[连接 " << fd
                                      << "] 异常："
                                      << e.what() << '\n';
                        }
                    }
                ));
            } catch (const std::exception& e) {
                // 创建任务失败，连接还没有交出去，需要主动关闭。
                ::close(fd);
                std::cerr << "无法创建工作任务："
                          << e.what() << '\n';
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "服务错误：" << e.what() << '\n';
        return 1;
    }
}