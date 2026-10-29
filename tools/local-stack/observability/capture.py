#!/usr/bin/env python3
"""Append Alertmanager webhook deliveries to a private local file; never forwards them."""

from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
import sys

LIMIT = 1024 * 1024


class Capture(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            length = -1
        if self.path != "/alerts" or not 0 < length <= LIMIT:
            self.send_error(400)
            return
        try:
            payload = json.loads(self.rfile.read(length))
        except ValueError:
            self.send_error(400)
            return
        line = json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n"
        descriptor = os.open(self.server.output, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            data = memoryview(line.encode())
            while data:
                data = data[os.write(descriptor, data):]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    server = HTTPServer(("127.0.0.1", 9099), Capture)
    server.output = sys.argv[1]
    server.serve_forever()
