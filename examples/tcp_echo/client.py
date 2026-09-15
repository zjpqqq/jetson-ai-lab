import socket
import uuid

from protocol import encode_message, recv_message


requests = [
    {
        "request_id": uuid.uuid4().hex,
        "text": "你好，我是客户端",
    },
    {
        "request_id": uuid.uuid4().hex,
        "text": "查询机器人状态",
    },
]

with socket.create_connection(("127.0.0.1", 9000), timeout=10) as sock:
    sock.settimeout(30)

    packet = b"".join(encode_message(item) for item in requests)
    sock.sendall(packet)

    for request in requests:
        response = recv_message(sock)

        if response.get("request_id") != request["request_id"]:
            raise ValueError("响应的 request_id 不匹配")

        print("服务端返回：", response)