import socket
import uuid

from protocol import encode_message, recv_message


with socket.create_connection(("127.0.0.1", 9001), timeout=5) as sock:
    # 大于服务端的 120 秒 HTTP 总超时。
    sock.settimeout(180)

    for text in [
        "你好，请用一句话介绍自己。",
        "用一句话解释什么是 TCP。",
    ]:
        request_id = uuid.uuid4().hex

        request = {
            "request_id": request_id,
            "text": text,
        }

        print("发送：", text, flush=True)
        sock.sendall(encode_message(request))

        response = recv_message(sock)

        if response.get("request_id") != request_id:
            raise ValueError("响应 request_id 不匹配")

        if response.get("ok") is True:
            print("模型回答：", response["result"])
        else:
            print("请求失败：", response.get("error"))

        print()