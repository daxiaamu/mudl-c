import hashlib
import http.server
import pathlib
import subprocess
import tempfile
import threading


DATA = bytes(range(256)) * 8192


class Handler(http.server.BaseHTTPRequestHandler):
    counts = {}
    lock = threading.Lock()

    def log_message(self, *_args):
        pass

    def _count(self):
        with self.lock:
            count = self.counts.get(self.path, 0) + 1
            self.counts[self.path] = count
            return count

    def _range(self, etag, total=None):
        value = self.headers.get("Range", "bytes=0-")[6:]
        start_text, end_text = value.split("-", 1)
        start = int(start_text)
        end = int(end_text) if end_text else len(DATA) - 1
        body = DATA[start : end + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{total or len(DATA)}")
        self.send_header("ETag", etag)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        count = self._count()
        if self.path == "/range":
            self._range('"stable"')
        elif self.path == "/changed":
            self._range('"probe"' if count == 1 else '"changed"')
        elif self.path == "/wrong-total":
            self._range('"stable"', len(DATA) if count == 1 else len(DATA) + 1)
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("ETag", '"chunked"')
            self.end_headers()
            try:
                for offset in range(0, len(DATA), 100_000):
                    chunk = DATA[offset : offset + 100_000]
                    self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii"))
                    self.wfile.write(chunk)
                    self.wfile.write(b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


def run(mudl, temp, url, name, *args, timeout=15):
    command = [str(mudl), "-d", str(temp), "-o", name,
               "--progress", "none", *args, url]
    return subprocess.run(command, capture_output=True, timeout=timeout)


def main():
    root = pathlib.Path(__file__).resolve().parents[1]
    mudl = root / "mudl.exe"
    expected = hashlib.sha256(DATA).digest()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"

    try:
        with tempfile.TemporaryDirectory() as directory:
            temp = pathlib.Path(directory)

            result = run(mudl, temp, base + "/range", "range.bin", "-c", "2")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert hashlib.sha256((temp / "range.bin").read_bytes()).digest() == expected

            (temp / "chunked.bin").write_bytes(DATA + b"stale trailing bytes")
            result = run(mudl, temp, base + "/chunked", "chunked.bin", "-c", "1")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert hashlib.sha256((temp / "chunked.bin").read_bytes()).digest() == expected

            result = run(mudl, temp, base + "/changed", "changed.bin",
                         "-c", "2", "--retries", "0")
            assert result.returncode != 0, "changed validator was accepted"

            result = run(mudl, temp, base + "/wrong-total", "wrong-total.bin",
                         "-c", "2", "--retries", "0")
            assert result.returncode != 0, "changed Content-Range total was accepted"

            result = run(mudl, temp, base + "/missing", "missing.bin")
            assert result.returncode != 0, "HTTP 404 was accepted"
    finally:
        server.shutdown()
        server.server_close()

    print("integration tests passed")


if __name__ == "__main__":
    main()
