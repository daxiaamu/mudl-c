import hashlib
import http.server
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading


DATA = bytes(range(256)) * 8192


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path != "/range":
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        value = self.headers.get("Range", "bytes=0-")[6:]
        start_text, end_text = value.split("-", 1)
        start = int(start_text)
        end = int(end_text) if end_text else len(DATA) - 1
        body = DATA[start:end + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{len(DATA)}")
        self.send_header("ETag", '"https-stable"')
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def run(mudl, directory, url, output):
    return subprocess.run(
        [str(mudl), "-d", str(directory), "-o", output,
         "--progress", "none", url],
        capture_output=True,
        timeout=30,
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_https.py CERT_PEM KEY_PEM")

    root = pathlib.Path(__file__).resolve().parents[1]
    mudl = root / "mudl.exe"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(sys.argv[1], sys.argv[2])

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    port = server.server_address[1]

    try:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            trusted_url = (
                "https://raw.githubusercontent.com/daxiaamu/mudl-c/main/LICENSE")
            result = run(mudl, temp, trusted_url, "trusted-license.txt")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert hashlib.sha256(
                (temp / "trusted-license.txt").read_bytes()).digest() == \
                   hashlib.sha256((root / "LICENSE").read_bytes()).digest()

            result = run(
                mudl, temp, f"https://localhost:{port}/range", "untrusted.bin")
            stderr = result.stderr.decode(errors="replace")
            assert result.returncode != 0, "self-signed certificate was accepted"
            assert "certificate chain is not trusted" in stderr, stderr
    finally:
        server.shutdown()
        server.server_close()

    print("HTTPS SChannel integration tests passed")


if __name__ == "__main__":
    main()
