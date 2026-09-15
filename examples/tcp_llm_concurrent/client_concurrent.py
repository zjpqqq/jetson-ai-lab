import socket
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

from protocol import encode_message, recv_message


# 两个客户端都连接成功后，再一起发送请求。
barrier = threading.Barrier(2)


def run_client(number, text):
    request_id = uuid.uuid4().hex

    with socket.create_connection(
        ("127.0.0.1", 9002), timeout=5
    ) as sock:
        sock.settimeout(180)

        barrier.wait(timeout=10)
        started = time.perf_counter()

        print(f"[客户端 {number}] 开始请求", flush=True)

        sock.sendall(encode_message({
            "request_id": request_id,
            "text": text,
        }))

        response = recv_message(sock)
        elapsed = time.perf_counter() - started

        if response.get("request_id") != request_id:
            raise ValueError("request_id 不匹配")

        return number, elapsed, response


with ThreadPoolExecutor(max_workers=2) as pool:
    tasks = [
        pool.submit( run_client, 1, "用一句话解释 TCP。"),
        pool.submit( run_client, 2, "用一句话解释 HTTP。"),
    ]

    for task in tasks:
        try:
            number, elapsed, response = task.result()

            print(f"\n[客户端 {number}] 耗时 {elapsed:.2f} 秒")

            if response.get("ok") is True:
                print("回答：", response["result"])
            else:
                print("失败：", response.get("error"))

        except Exception as exc:
            print("测试异常：", exc)