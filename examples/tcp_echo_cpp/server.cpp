#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

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
    // 每次阻塞收发的超时，不是整个请求的总时限。
    timeval timeout{};
    timeout.tv_sec = 30;

    check(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)), "SO_RCVTIMEO");
    check(::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       &timeout, sizeof(timeout)), "SO_SNDTIMEO");

    while (true) {
        // ① 读取 4 字节消息头
        std::uint32_t network_size = 0;

        if (!recv_exact(fd, &network_size, sizeof(network_size))) {
            std::cout << "客户端正常断开\n";
            return;
        }

        // ② 网络字节序转换为本机字节序
        const std::uint32_t size = ntohl(network_size);

        if (size == 0 || size > MAX_BODY) {
            throw std::runtime_error("非法消息长度");
        }

        // ③ 按长度读取完整 JSON 正文
        std::string body(size, '\0');

        if (!recv_exact(fd, body.data(), body.size())) {
            throw std::runtime_error("收到消息头后，对端断开");
        }

        const json request = json::parse(body);

        if (!request.is_object()) {
            throw std::runtime_error("消息必须是 JSON 对象");
        }

        std::cout << "收到：" << request.dump() << '\n';

        // ④ 暂时回显，后续在这里调用模型
        const json response = {
            {"request_id", request.value("request_id", json(nullptr))},
            {"ok", true},
            {"result", request.value("text", json(""))}
        };

        // ⑤ 编码响应：4 字节长度头 + UTF-8 JSON
        const std::string output = response.dump();

        if (output.empty() || output.size() > MAX_BODY) {
            throw std::runtime_error("响应长度超限");
        }

        const std::uint32_t output_size =
                htonl(static_cast<std::uint32_t>(output.size()));

        send_all(fd, &output_size, sizeof(output_size));
        send_all(fd, output.data(), output.size());
    }
}

int main() {
    try {
        Socket server(::socket(AF_INET, SOCK_STREAM, 0));

        const int reuse = 1;
        check(::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse)), "SO_REUSEADDR");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9000);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        check(::bind(server.get(),
                     reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)), "bind");
        check(::listen(server.get(), 5), "listen");

        std::cout << "C++ TCP 服务监听 127.0.0.1:9000\n";

        while (true) {
            const int fd = ::accept(server.get(), nullptr, nullptr);

            if (fd < 0 && errno == EINTR) {
                continue;
            }

            Socket client(fd);
            std::cout << "客户端已连接\n";

            try {
                handle_client(client.get());
            } catch (const std::exception& e) {
                std::cerr << "连接结束：" << e.what() << '\n';
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "服务错误：" << e.what() << '\n';
        return 1;
    }
}