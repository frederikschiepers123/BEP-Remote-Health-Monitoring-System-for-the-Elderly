"""Health endpoint (CLAUDE.md §14.2). Stdlib http.server — no extra dependency.

GET /health → JSON {status, broker, queue_depth, version}. Docker's healthcheck
and operators poll it. `status_provider` is a callable returning the dict.
"""
from __future__ import annotations

import json
import logging
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log = logging.getLogger(__name__)


def start_health_server(port: int, status_provider) -> ThreadingHTTPServer:
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            if self.path.rstrip("/") not in ("/health", ""):
                self.send_error(404)
                return
            try:
                body = json.dumps(status_provider()).encode()
                code = 200
            except Exception as exc:   # never let the probe crash the thread
                body = json.dumps({"status": "unhealthy", "error": str(exc)}).encode()
                code = 500
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):  # silence default stderr access log
            pass

    # Bind to localhost only: the endpoint exposes internal queue depth, and the
    # Docker healthcheck curls http://localhost:9100/health anyway. Do not expose
    # it on all interfaces (§14.2).
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, name="health", daemon=True).start()
    log.info("health endpoint on 127.0.0.1:%d/health", port)
    return srv
