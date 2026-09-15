import socket

from protocol import encode_message, recv_message


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", 9000))
        server.listen(5)

        print("TCP 服务监听 127.0.0.1:9000")

        while True:
            conn, address = server.accept()
            print("客户端连接：", address)

            with conn:
                conn.settimeout(30)

                try:
                    while True:
                        request = recv_message(conn)
                        print("收到：", request)

                        response = {
                            "request_id": request.get("request_id"),
                            "ok": True,
                            "result": request.get("text", ""),
                        }

                        conn.sendall(encode_message(response))

                except EOFError:
                    print("客户端断开")
                except (OSError, ValueError) as exc:
                    print("连接结束：", exc)


if __name__ == "__main__":
    main()