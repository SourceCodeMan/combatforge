#!/usr/bin/env python3
"""Serve this folder for the box to DOWNLOAD builds, and accept PUT UPLOADS into inbox/.

Drop-in replacement for `python -m http.server` in the deploy flow. The stock module is GET-only,
so the box can pull a build but has no way to push its logs back. This adds exactly one thing:
PUT under /inbox/.

    # PC (in Deploy\\pilot\\serve)
    python serve-and-receive.py
    cloudflared tunnel --url http://localhost:8000

    # box: deploy a build (download, unchanged)      -> Deploy-OnBox.ps1
    # box: send logs back (upload, needs this file)  -> Export-ServerLogs.ps1

Binds to localhost only: cloudflared connects locally, so there is no reason to listen on the LAN.
Uploads are restricted to /inbox/, the filename is sanitized (no traversal), and there is a size
ceiling. The tunnel URL is unguessable and short-lived, but close the window when you are done -
while it is open, anyone holding the URL can write into inbox/.
"""

import http.server
import os
import pathlib
import re
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
ROOT = pathlib.Path(__file__).resolve().parent
INBOX = ROOT / "inbox"
MAX_UPLOAD = 2 * 1024 ** 3          # 2 GB ceiling
SAFE_NAME = re.compile(r"[A-Za-z0-9._-]{1,128}\Z")
CHUNK = 1 << 20


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_PUT(self):
        if not self.path.startswith("/inbox/"):
            self.send_error(403, "PUT is only allowed under /inbox/")
            return

        name = os.path.basename(self.path)
        if not name or not SAFE_NAME.match(name):
            self.send_error(400, "bad filename")
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "bad Content-Length")
            return
        if length <= 0 or length > MAX_UPLOAD:
            self.send_error(413, "missing or oversized body")
            return

        INBOX.mkdir(exist_ok=True)
        dest = INBOX / name
        remaining = length
        try:
            with open(dest, "wb") as fh:
                while remaining > 0:
                    chunk = self.rfile.read(min(CHUNK, remaining))
                    if not chunk:
                        break
                    fh.write(chunk)
                    remaining -= len(chunk)
        except OSError as exc:
            dest.unlink(missing_ok=True)
            self.send_error(500, f"write failed: {exc}")
            return

        if remaining:
            # Truncated transfer: do not leave a half file that looks like a good capture.
            dest.unlink(missing_ok=True)
            self.send_error(400, "upload truncated")
            return

        print(f"[recv] {name}  {length / 1048576:.1f} MB  -> {dest}", flush=True)
        self.send_response(201, "Created")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        # Quieter than the default: one line per request, no client-address noise.
        print(f"[{self.command}] {self.path}", flush=True)


def main():
    os.chdir(ROOT)
    INBOX.mkdir(exist_ok=True)
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", PORT), Handler) as httpd:
        print(f"serving  {ROOT}")
        print(f"uploads  -> {INBOX}")
        print(f"listening on http://127.0.0.1:{PORT}   (Ctrl+C to stop)", flush=True)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")


if __name__ == "__main__":
    main()
