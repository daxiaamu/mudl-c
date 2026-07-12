import hashlib
import http.server
import pathlib
import struct
import subprocess
import tempfile
import threading
import zlib


DATA = bytes(range(256)) * 8192
FLAKY_DATA = DATA * 17
EXPAND_DATA = DATA * 8


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

    def _range(self, etag, total=None, data=DATA):
        value = self.headers.get("Range", "bytes=0-")[6:]
        start_text, end_text = value.split("-", 1)
        start = int(start_text)
        end = int(end_text) if end_text else len(data) - 1
        body = data[start : end + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{total or len(data)}")
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
        elif self.path == "/flaky":
            if count == 1 or count > 33:
                self._range('"stable"', data=FLAKY_DATA)
            else:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
        elif self.path == "/expand":
            self._range('"expand"', data=EXPAND_DATA)
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

            result = run(mudl, temp, base + "/flaky", "flaky.bin",
                         "-c", "32", "--retries", "1", timeout=30)
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert hashlib.sha256((temp / "flaky.bin").read_bytes()).digest() == \
                   hashlib.sha256(FLAKY_DATA).digest()

            resume_file = temp / "resume.bin"
            partial = len(DATA) // 2
            with resume_file.open("wb") as stream:
                stream.write(DATA[:partial])
                stream.truncate(len(DATA))
            validator = b'etag:"stable"'
            header = struct.pack(
                "<IIqIIIQ256s", 0x4D55444D, 3, len(DATA), 1, 1, 0, 0,
                validator.ljust(256, b"\0"))
            segment = struct.pack(
                "<IqqqIII", 0, 0, len(DATA) - 1, partial, 1, 0,
                zlib.crc32(DATA[:partial]) & 0xFFFFFFFF)
            (temp / "resume.bin.segments").write_bytes(header + segment)
            command = [str(mudl), "-d", str(temp), "-o", "resume.bin",
                       "-c", "1", "--progress", "line", base + "/range"]
            result = subprocess.run(command, capture_output=True, timeout=15)
            output = result.stdout.decode(errors="replace")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert "Downloaded 2.0 MB (avg " in output, output
            assert "Downloaded 1.0 MB" not in output, output
            assert hashlib.sha256(resume_file.read_bytes()).digest() == expected

            expanded_file = temp / "expanded.bin"
            expanded_partial = 128 * 1024
            with expanded_file.open("wb") as stream:
                stream.write(EXPAND_DATA[:expanded_partial])
                stream.truncate(len(EXPAND_DATA))
            expanded_validator = b'etag:"expand"'
            expanded_header = struct.pack(
                "<IIqIIIQ256s", 0x4D55444D, 3, len(EXPAND_DATA), 1, 1, 0, 0,
                expanded_validator.ljust(256, b"\0"))
            expanded_segment = struct.pack(
                "<IqqqIII", 0, 0, len(EXPAND_DATA) - 1, expanded_partial, 1, 0,
                zlib.crc32(EXPAND_DATA[:expanded_partial]) & 0xFFFFFFFF)
            (temp / "expanded.bin.segments").write_bytes(
                expanded_header + expanded_segment)
            before = Handler.counts.get("/expand", 0)
            command = [str(mudl), "-d", str(temp), "-o", "expanded.bin",
                       "-c", "8", "--progress", "none", base + "/expand"]
            result = subprocess.run(command, capture_output=True, timeout=15)
            requests = Handler.counts.get("/expand", 0) - before
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert requests >= 8, f"resume used only {requests} range requests"
            assert hashlib.sha256(expanded_file.read_bytes()).digest() == \
                   hashlib.sha256(EXPAND_DATA).digest()

            mixed_file = temp / "mixed.bin"
            boundary = len(EXPAND_DATA) // 2
            mixed_partial = 128 * 1024
            with mixed_file.open("wb") as stream:
                stream.write(EXPAND_DATA[:boundary + mixed_partial])
                stream.truncate(len(EXPAND_DATA))
            mixed_header = struct.pack(
                "<IIqIIIQ256s", 0x4D55444D, 3, len(EXPAND_DATA), 2, 2, 0, 0,
                expanded_validator.ljust(256, b"\0"))
            complete_segment = struct.pack(
                "<IqqqIII", 0, 0, boundary - 1, boundary, 2, 0,
                zlib.crc32(EXPAND_DATA[:boundary]) & 0xFFFFFFFF)
            partial_segment = struct.pack(
                "<IqqqIII", 1, boundary, len(EXPAND_DATA) - 1,
                mixed_partial, 1, 0,
                zlib.crc32(EXPAND_DATA[boundary:boundary + mixed_partial]) & 0xFFFFFFFF)
            (temp / "mixed.bin.segments").write_bytes(
                mixed_header + complete_segment + partial_segment)
            command = [str(mudl), "-d", str(temp), "-o", "mixed.bin",
                       "-c", "8", "--progress", "line", base + "/expand"]
            result = subprocess.run(command, capture_output=True, timeout=15)
            output = result.stdout.decode(errors="replace")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert "101.0%" not in output and "100.0%" in output, output
            assert hashlib.sha256(mixed_file.read_bytes()).digest() == \
                   hashlib.sha256(EXPAND_DATA).digest()

            lowered_file = temp / "lowered.bin"
            with lowered_file.open("wb") as stream:
                stream.write(EXPAND_DATA[:boundary + mixed_partial])
                stream.truncate(len(EXPAND_DATA))
            (temp / "lowered.bin.segments").write_bytes(
                mixed_header + complete_segment + partial_segment)
            command = [str(mudl), "-d", str(temp), "-o", "lowered.bin",
                       "-c", "1", "--progress", "line", base + "/expand"]
            result = subprocess.run(command, capture_output=True, timeout=15)
            output = result.stdout.decode(errors="replace")
            assert result.returncode == 0, result.stderr.decode(errors="replace")
            assert "Resuming from segments.bin (2 segments, 1 pending)" in output, output
            assert "Method:  segmented (1 connection)" in output, output
            assert "100.0%" in output, output
            assert hashlib.sha256(lowered_file.read_bytes()).digest() == \
                   hashlib.sha256(EXPAND_DATA).digest()

            result = run(mudl, temp, base + "/missing", "missing.bin")
            assert result.returncode != 0, "HTTP 404 was accepted"
    finally:
        server.shutdown()
        server.server_close()

    print("integration tests passed")


if __name__ == "__main__":
    main()
