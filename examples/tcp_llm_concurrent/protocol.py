import json
import struct

MAX_BODY = 64 * 1024  # 本练习限制单条 JSON 为 64 KiB


def recv_exact(sock, size):
    """循环接收，直到收齐 size 字节。"""
    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            raise EOFError("对端已关闭连接")

        data.extend(chunk)

    return bytes(data)


def encode_message(message):
    body = json.dumps(message, ensure_ascii=False).encode("utf-8")

    if not 0 < len(body) <= MAX_BODY:
        raise ValueError("消息长度超限")

    # !I：网络字节序，也就是大端；4 字节无符号整数
    header = struct.pack("!I", len(body))
    return header + body


def recv_message(sock):
    header = recv_exact(sock, 4)
    size = struct.unpack("!I", header)[0]

    if not 0 < size <= MAX_BODY:
        raise ValueError("非法消息长度")

    body = recv_exact(sock, size)
    message = json.loads(body.decode("utf-8"))

    if not isinstance(message, dict):
        raise ValueError("消息必须是 JSON 对象")

    return message