import hashlib
import http.server
import pathlib
import select
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


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


class TLSServer(http.server.ThreadingHTTPServer):
    def __init__(self, address, handler, context):
        self.tls_context = context
        super().__init__(address, handler)

    def get_request(self):
        sock, address = super().get_request()
        return self.tls_context.wrap_socket(sock, server_side=True), address


class ConnectProxy(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    seen = []

    def log_message(self, *_args):
        pass

    def do_CONNECT(self):
        self.seen.append(self.path)
        host, port = self.path.rsplit(":", 1)
        upstream = socket.create_connection((host, int(port)), timeout=10)
        self.send_response(200, "Connection Established")
        self.send_header("Proxy-Agent", "mudl-test")
        self.end_headers()
        self.wfile.flush()

        sockets = [self.connection, upstream]
        try:
            while True:
                ready, _, _ = select.select(sockets, [], [], 10)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    target = upstream if source is self.connection else self.connection
                    target.sendall(data)
        finally:
            upstream.close()


def run(mudl, directory, url, output, *args):
    return subprocess.run(
        [str(mudl), "-d", str(directory), "-o", output,
         "--progress", "none", *args, url],
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

    server = TLSServer(("127.0.0.1", 0), Handler, context)
    proxy = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ConnectProxy)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    proxy_thread = threading.Thread(target=proxy.serve_forever, daemon=True)
    thread.start()
    proxy_thread.start()
    port = server.server_address[1]

    try:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)
            trusted_url = (
                "https://raw.githubusercontent.com/daxiaamu/mudl-c/main/LICENSE")
            result = run(mudl, temp, trusted_url, "trusted-license.txt")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            downloaded = (temp / "trusted-license.txt").read_bytes().replace(
                b"\r\n", b"\n")
            expected = (root / "LICENSE").read_bytes().replace(b"\r\n", b"\n")
            assert hashlib.sha256(downloaded).digest() == \
                   hashlib.sha256(expected).digest()

            result = run(
                mudl, temp, f"https://localhost:{port}/range", "untrusted.bin")
            stderr = result.stderr.decode(errors="replace")
            assert result.returncode != 0, "self-signed certificate was accepted"
            assert "certificate chain is not trusted" in stderr, stderr

            started = time.monotonic()
            result = run(
                mudl, temp, f"https://localhost:{port}/range",
                "proxy-untrusted.bin", "--proxy",
                f"http://127.0.0.1:{proxy.server_port}")
            elapsed = time.monotonic() - started
            stderr = result.stderr.decode(errors="replace")
            assert result.returncode != 0, "self-signed proxy tunnel was accepted"
            assert "certificate chain is not trusted" in stderr, stderr
            assert ConnectProxy.seen[-1] == f"localhost:{port}", ConnectProxy.seen
            assert elapsed < 10, f"CONNECT header parsing stalled for {elapsed:.1f}s"
    finally:
        server.shutdown()
        server.server_close()
        proxy.shutdown()
        proxy.server_close()

    print("HTTPS SChannel integration tests passed")


if __name__ == "__main__":
    main()
