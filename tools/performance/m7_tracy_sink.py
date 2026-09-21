#!/usr/bin/env python3
"""最小 Tracy profiler 角色（握手 + 持续接收/丢弃），用于 M7-02 的开销实验。

为什么需要它：官方 `tracy-capture` / Profiler GUI 的 CMake 依赖 CPM 从 GitHub 拉取
capstone/freetype/zstd（本机 git 传输不可用），无法离线构建。本脚本只实现协议中最小的
服务端角色（Tracy 客户端监听端口、由 profiler 主动连接）：

    1. 连接 127.0.0.1:<port>（客户端在 8086 监听）
    2. 发送 8 字节 shibboleth "TracyPrf" + uint32 ProtocolVersion(76)
    3. 读取 1 字节 HandshakeStatus（1 = HandshakeWelcome 表示被接受）
    4. 之后持续 recv 并丢弃，统计收到的字节数与时长

它**不解析**协议内容（事件是 LZ4 压缩流），因此不能产出 .tracy 文件；它的作用是让客户端
进入"已连接"状态并真实发送数据，从而测量客户端侧（编码/排队/发送）的开销。观测覆盖
（zone/lock/memory/counter 是否真的发出）由 facade 的事件计数器与代码审计证明。

用法：
  python tools/performance/m7_tracy_sink.py --port 8086 --timeout 600 --report out/m7-02/sink.json
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

SHIBBOLETH = b"TracyPrf"
PROTOCOL_VERSION = 76
HANDSHAKE_WELCOME = 1


def write_report(path: str, payload: dict) -> None:
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(payload, stream, indent=2)
        stream.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8086)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--connect-timeout", type=float, default=120.0)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()

    report = {
        "connected": False,
        "handshakeStatus": 0,
        "bytes": 0,
        "seconds": 0.0,
        "port": args.port,
        "protocolVersion": PROTOCOL_VERSION,
    }
    started = time.time()
    deadline = started + args.connect_timeout
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=2.0)
            break
        except OSError:
            time.sleep(0.2)
    if sock is None:
        report["error"] = "client listener not reachable"
        write_report(args.report, report)
        print("sink: connect failed", file=sys.stderr)
        return 1

    sock.settimeout(5.0)
    sock.sendall(SHIBBOLETH)
    sock.sendall(struct.pack("<I", PROTOCOL_VERSION))
    status = sock.recv(1)
    if not status or status[0] != HANDSHAKE_WELCOME:
        report["error"] = f"handshake rejected: {status!r}"
        write_report(args.report, report)
        print(f"sink: handshake rejected {status!r}", file=sys.stderr)
        return 1
    report["connected"] = True
    report["handshakeStatus"] = int(status[0])

    total = 0
    while time.time() - started < args.timeout:
        try:
            chunk = sock.recv(1 << 16)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        total += len(chunk)
    sock.close()

    report["bytes"] = total
    report["seconds"] = round(time.time() - started, 3)
    write_report(args.report, report)
    print(f"sink: connected bytes={total} seconds={report['seconds']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
