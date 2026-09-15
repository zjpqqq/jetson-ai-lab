import socket
import time
import uuid

from protocol import encode_message, recv_message


request_id = uuid.uuid4().hex
text = "请分三点解释TCP，每点用一到两句话。"

with socket.create_connection(
    ("127.0.0.1", 9003), timeout=5
) as sock:
    sock.settimeout(180)

    started = time.perf_counter()
    first_piece_time = None
    pieces = 0

    sock.sendall(encode_message({
        "request_id": request_id,
        "text": text,
    }))

    print("模型回答：", end="", flush=True)

    while True:
        message = recv_message(sock)

        if message.get("request_id") != request_id:
            raise ValueError("request_id 不匹配")

        event_type = message.get("type")

        if event_type == "delta":
            piece = message["text"]

            if piece:
                if first_piece_time is None:
                    first_piece_time = (
                        time.perf_counter() - started
                    )

                pieces += 1
                print(piece, end="", flush=True)

        elif event_type == "done":
            elapsed = time.perf_counter() - started

            print("\n\n生成结束")
            print("结束原因：", message.get("finish_reason"))
            print("正文片段数：", pieces)

            if first_piece_time is not None:
                print(
                    f"首个正文片段耗时："
                    f"{first_piece_time:.2f} 秒"
                )

            print(f"总耗时：{elapsed:.2f} 秒")
            break

        elif event_type == "error":
            print("\n\n请求失败：", message.get("error"))
            print("此前显示的内容可能是不完整回答。")
            break

        else:
            raise ValueError(f"未知事件类型：{event_type}")