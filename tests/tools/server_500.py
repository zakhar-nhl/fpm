#!/usr/bin/env python3
"""Minimal HTTP server that answers every request with HTTP 500.
Used by tests/run_tests.sh to exercise mirror fallback on server errors."""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        body = b"internal server error"
        self.send_response(500)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # silence
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18790
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()