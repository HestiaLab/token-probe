#!/usr/bin/env python3
"""平台传输层的超时行为测试。

启动一个只接受连接、永不响应的回环服务器，再以较短的 --timeout 调用 CLI。
断言 CLI 在超时后失败，而不是一直挂起。
"""

import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

_TIMEOUT_MS = 300
_DEADLINE_S = 8.0


class _Blackhole:
    """接受连接后保持打开，从不发送响应。"""

    def __init__(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(8)
        self._sock.settimeout(0.2)
        self.port = self._sock.getsockname()[1]
        self._stop = threading.Event()
        self._connections = []
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._connections.append(conn)

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)
        for conn in self._connections:
            try:
                conn.close()
            except OSError:
                pass
        self._sock.close()


def _run(exe: Path) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items() if k != "DEEPSEEK_API_KEY"}
    return subprocess.run(
        [str(exe), "--provider", "deepseek", "--api-key", "sk-test",
         "--base-url", f"http://127.0.0.1:{server.port}",
         "--timeout", str(_TIMEOUT_MS)],
        capture_output=True,
        env=env,
        timeout=15,
    )


def main() -> int:
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build/ninja-debug/bin/tokenprobe.exe")
    if not exe.is_file():
        print(f"FAIL: CLI not found: {exe}", file=sys.stderr)
        return 1

    started = time.monotonic()
    proc = _run(exe)
    elapsed = time.monotonic() - started
    output = (proc.stdout + proc.stderr).decode("utf-8", "replace")

    if "no usable network transport" in output:
        print("SKIP: 本构建没有平台传输层")
        return 0
    if proc.returncode != 1:
        print(f"FAIL: expected exit 1, got {proc.returncode}\n{output}", file=sys.stderr)
        return 1
    if "TIMEOUT" not in output:
        print(f"FAIL: expected a TIMEOUT verdict\n{output}", file=sys.stderr)
        return 1
    if elapsed >= _DEADLINE_S:
        print(f"FAIL: timeout took {elapsed:.2f}s, expected < {_DEADLINE_S}s",
              file=sys.stderr)
        return 1

    print(f"OK: timed out after {elapsed:.2f}s")
    return 0


if __name__ == "__main__":
    server = _Blackhole()
    try:
        sys.exit(main())
    finally:
        server.close()
