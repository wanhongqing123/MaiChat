#!/usr/bin/env python3
"""Exercise only disposable loopback TLS signer/OSS routes, with no real account."""
import http.server
import json
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

OBJECT = "avatars/account_a/2026-11-03/123e4567-e89b-42d3-a456-426614174000.png"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def reply(self, code, value, headers=None):
        body = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def do_POST(self):
        path = urlsplit(self.path).path
        if not path.endswith("/sign-upload"):
            return self.reply(404, {})
        mode = path.split("/")[1]
        self.server.signs.append(mode)
        if self.headers.get("Authorization") != "Bearer local-avatar-fixture-token":
            return self.reply(401, {})
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert body == {"filename": "avatar.png", "size_bytes": 33, "purpose": "im_avatar"}, body
        origin = f"https://localhost:{self.server.server_port}"
        if mode == "sign_redirect":
            return self.reply(302, {}, {"Location": origin + "/must-not-follow"})
        if mode == "sign_failure":
            return self.reply(503, {"error": "local fixture failure"})
        if mode == "non_json":
            return self.reply(200, b"not json")
        if mode == "large":
            return self.reply(200, b" " * 40000)
        signed = {
            "object_key": OBJECT,
            "avatar_url": origin + "/" + mode + "/" + OBJECT,
            "upload_url": origin + "/" + OBJECT + "?case=" + mode + "&Signature=local-fixture-signature",
            "read_url": origin + "/" + OBJECT + "?Signature=not-for-model",
            "upload_headers": {"Content-Type": "image/png"},
        }
        if mode == "owner":
            signed["object_key"] = signed["object_key"].replace("account_a", "account_b")
        if mode == "object":
            signed["upload_url"] = origin + "/avatars/account_a/other.png?Signature=local-fixture-signature"
        if mode == "host":
            signed["upload_url"] = signed["upload_url"].replace("localhost:", "localhost.evil.invalid:")
        if mode == "headers":
            signed["upload_headers"]["Authorization"] = "not-allowed"
        if mode == "signed_avatar":
            signed["avatar_url"] += "?Signature=not-for-model"
        self.reply(200, signed)

    def do_PUT(self):
        request = urlsplit(self.path)
        self.server.puts.append(request.query)
        assert request.path == "/" + OBJECT
        assert self.headers.get("Authorization") is None
        assert self.headers.get("Cookie") is None
        assert self.headers.get("Content-Type") == "image/png"
        data = self.rfile.read(int(self.headers["Content-Length"]))
        assert data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) == 33
        if "case=put_redirect" in request.query:
            return self.reply(302, {}, {"Location": "/must-not-follow"})
        if "case=put_failure" in request.query:
            return self.reply(500, {})
        if "case=slow" in request.query:
            time.sleep(2)
        self.reply(200, {})

    def do_GET(self):
        self.server.followed += 1
        self.reply(500, {})


def main():
    with tempfile.TemporaryDirectory(prefix="maichat-avatar-tls-") as directory:
        root = Path(directory)
        key, cert = root / "server.key", root / "server.crt"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.signs, server.puts, server.followed = [], [], 0
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = subprocess.run([sys.argv[1], f"https://localhost:{server.server_port}", str(cert)],
                                    timeout=30, check=False)
            assert result.returncode == 0, result.returncode
            assert server.followed == 0, "redirect was followed"
            assert "invalid" not in server.signs and "pre_cancel" not in server.signs
            assert len(server.puts) == 4, server.puts
            print("avatar TLS routes, credential separation and bounded transfers passed")
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=3)


if __name__ == "__main__":
    main()
