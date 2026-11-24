#!/usr/bin/env python3
"""Loopback-only fixture; run a host test command or serve for a BOL smoke test."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import subprocess
import threading
import time


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body=b"", **headers):
        try:
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            for key, value in headers.items():
                self.send_header(key.replace("_", "-"), value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path == "/redirect":
            self.reply(302, Location="/get")
        elif self.path == "/slow":
            time.sleep(0.7)
            self.reply(200, b"slow")
        elif self.path == "/get":
            self.reply(200, b'{"ok":true}', X_Fixture="scriptclient")
        elif self.path == "/bytes":
            self.reply(200, bytes([0, 128, 255, 10]))
        elif self.path == "/large":
            self.reply(200, b"x" * 4096)
        else:
            self.reply(418, b"not a success")

    do_HEAD = do_GET

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > 8 * 1024 * 1024:
            self.reply(413)
            return
        self.reply(201, self.rfile.read(length),
                   X_Echo=self.headers.get("X-Fixture-Request", ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
        url = f"http://127.0.0.1:{server.server_port}"
        print(f"ScriptClient HTTP fixture at {url}", flush=True)
        if not args.command:
            server.serve_forever()
        else:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                command = args.command[1:] if args.command[0] == "--" else args.command
                return subprocess.call(command, env={**os.environ, "STAGEB_HTTP_FIXTURE": url})
            finally:
                server.shutdown()
                thread.join()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
