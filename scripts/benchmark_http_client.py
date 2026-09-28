#!/usr/bin/env python3
"""Measure repeated Tilt HTTP calls against a local keep-alive server."""

from __future__ import annotations

import argparse
import http.server
import os
import socket
import statistics
import subprocess
import tempfile
import threading
import time
from pathlib import Path


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        # Tiny header/body writes otherwise trigger a 40 ms delayed-ACK/Nagle
        # artifact on some loopback kernels, obscuring client overhead.
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *_args):
        pass

    def do_GET(self):
        body = b'{"value":1}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tilt", type=Path)
    parser.add_argument("--requests", type=int, default=40)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    if args.requests < 1 or args.repetitions < 1:
        parser.error("requests and repetitions must be positive")

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="tilt-http-bench-") as tmp:
                program = Path(tmp) / "http.tilt"
                program.write_text(f'''funcao principal:
  i = 0
  total = 0
  enquanto i < {args.requests}:
    r = http_get_json "http://127.0.0.1:{server.server_port}/value"
    total = total + r.value
    i = i + 1
  imprimir total
''')
                print("backend,requests,median_ms")
                for backend in ("libcurl", "cli"):
                    samples = []
                    for _ in range(args.repetitions):
                        start = time.perf_counter_ns()
                        result = subprocess.run(
                            [str(args.tilt.resolve()), "executar", str(program)],
                            env={**os.environ, "TILT_HTTP_BACKEND": backend},
                            capture_output=True, text=True, check=True)
                        if str(args.requests) not in result.stdout:
                            raise RuntimeError(f"unexpected Tilt output: {result.stdout}")
                        samples.append((time.perf_counter_ns() - start) / 1_000_000)
                    print(f"{backend},{args.requests},{statistics.median(samples):.3f}")
        finally:
            server.shutdown()
            thread.join()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
