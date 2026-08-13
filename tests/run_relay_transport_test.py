#!/usr/bin/env python3
"""Run the real relay and the C++ mod transport client against localhost."""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: run_relay_transport_test.py RELAY CLIENT_TEST")
    relay = Path(sys.argv[1]).resolve()
    client = Path(sys.argv[2]).resolve()
    port = reserve_port()
    process = subprocess.Popen(
        [
            str(relay),
            "--host", "127.0.0.1",
            "--port", str(port),
            "--public-host", "127.0.0.1",
            "--public-port", str(port),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        if process.stdout is None:
            raise RuntimeError("relay output was not captured")
        first_line = process.stdout.readline().strip()
        if not first_line.startswith("Relay code: TP1-"):
            raise RuntimeError(f"relay startup failed: {first_line!r}")
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.05):
                    break
            except OSError:
                time.sleep(0.01)
        else:
            raise TimeoutError("relay did not listen")
        result = subprocess.run(
            [str(client), str(port)], text=True, capture_output=True, timeout=10
        )
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
        if process.stdout is not None:
            process.stdout.close()


if __name__ == "__main__":
    raise SystemExit(main())
