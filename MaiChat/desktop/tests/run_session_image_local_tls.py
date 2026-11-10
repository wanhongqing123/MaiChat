#!/usr/bin/env python3
"""Verify ordinary Desktop image readers against disposable private TLS storage."""

import importlib.util
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit("expected desktop_session_sync_host_test executable [screenshot]")
    fixture_path = Path(__file__).parents[2] / "Network/tests/run_cloud_drive_local_tls.py"
    spec = importlib.util.spec_from_file_location("session_image_tls_fixture", fixture_path)
    shared = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shared)
    with tempfile.TemporaryDirectory(prefix="maichat-session-image-") as directory:
        certificate = Path(directory) / "server.pem"
        key = Path(directory) / "server.key"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost", "-addext",
                        "subjectAltName=DNS:localhost,IP:127.0.0.1", "-keyout", str(key), "-out",
                        str(certificate)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = shared.Fixture(("127.0.0.1", 0))
        server.temporary_lifetime_ms = 5000
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(certificate), str(key))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            environment = os.environ.copy()
            environment["MAICHAT_IMAGE_TEST_ENDPOINT"] = f"https://localhost:{server.server_port}"
            environment["MAICHAT_IMAGE_TEST_CA"] = str(certificate)
            if len(sys.argv) == 3:
                environment["MAICHAT_IMAGE_TEST_SCREENSHOT"] = str(Path(sys.argv[2]).resolve())
            function = "temporaryImageLocalTlsCachesAcrossExpiryAndOwnerOffline"
            if os.name == "nt":
                # The repository wrapper supplies Qt's DLL directory and the
                # nonmodal Windows error mode. Keep fixture credentials only
                # in this child's environment, never in generated command text.
                desktop = Path(__file__).parents[1]
                build = Path(sys.argv[1]).resolve().parents[1]
                log = (Path(sys.argv[2]).resolve().with_suffix(".qtest.txt") if len(sys.argv) == 3
                       else Path(directory) / "desktop-image.qtest.txt")
                quote = lambda value: "'" + str(value).replace("'", "''") + "'"
                command = ("& " + quote(desktop / "scripts/run-tests-windows.ps1") +
                           " -BuildDirectory " + quote(os.path.relpath(build, desktop)) +
                           " -TestRegex '^desktop_session_sync_host_test$' -TestFunction @(" +
                           ",".join(quote(value) for value in (function, "-o", str(log) + ",txt")) + ")")
                environment["QT_QPA_PLATFORM"] = "windows"
                arguments = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command]
            else:
                log = None
                arguments = [sys.argv[1], "-platform", "offscreen", function]
            result = subprocess.run(arguments, env=environment, check=False, timeout=45)
            if log and log.is_file():
                print(log.read_text(encoding="utf-8", errors="replace"))
            if result.returncode:
                return result.returncode
            if server.sessions != 1 or server.read_requests != 1:
                raise AssertionError(f"unexpected object/read requests: {server.sessions}/{server.read_requests}")
            print("Desktop temporary image: original owner offline/relogged, cached after expiry, "
                  "other account uncached expired; exactly one init and one signed read")
            return 0
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
