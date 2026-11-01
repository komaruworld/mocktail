import hashlib
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading


def read_headers(connection):
    data = b""
    while not data.endswith(b"\r\n\r\n"):
        chunk = connection.recv(1)
        if not chunk:
            raise RuntimeError("connection closed before headers")
        data += chunk
        if len(data) > 65536:
            raise RuntimeError("headers too large")
    return data.decode()


def main():
    fixture, openssl = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="mocktail-download-") as directory:
        root = Path(directory)
        cert, key = root / "cert.pem", root / "key.pem"
        subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-subj", "/CN=download.test", "-addext",
                        "subjectAltName=DNS:download.test", "-keyout", str(key),
                        "-out", str(cert)], check=True, capture_output=True)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        original = b"PK\x03\x04" + b"a" * 65532
        changed = b"PK\x03\x04" + b"b" * 65532

        def run(destination, responses, success, attempts=1, path="/archive"):
            failures, requests = [], []
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen()
                listener.settimeout(5)

                def serve():
                    try:
                        for response in responses:
                            connection, _ = listener.accept()
                            with connection:
                                connection.settimeout(5)
                                assert read_headers(connection).startswith("CONNECT download.test:443 ")
                                connection.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                                with context.wrap_socket(connection, server_side=True) as tls:
                                    requests.append(read_headers(tls))
                                    try:
                                        tls.sendall(response)
                                    except (ssl.SSLError, ConnectionError):
                                        pass
                    except BaseException as error:
                        failures.append(error)

                thread = threading.Thread(target=serve, daemon=True)
                thread.start()
                env = {k: v for k, v in os.environ.items() if not k.lower().endswith("_proxy")}
                env["https_proxy"] = f"http://127.0.0.1:{listener.getsockname()[1]}"
                process = subprocess.run([fixture, "https://download.test" + path,
                                          str(destination), str(cert), str(attempts)],
                                         env=env, capture_output=True, text=True, timeout=15)
                thread.join(6)
                assert not thread.is_alive(), "server did not finish"
                assert not failures, (failures, process.stderr, requests)
                assert (process.returncode == 0) == success, process.stderr
                if success:
                    assert process.stdout.strip() == hashlib.sha256(destination.read_bytes()).hexdigest()
                return requests

        def response(body, status=200, etag='"v1"', length=None, extra=""):
            return (f"HTTP/1.1 {status} Result\r\nContent-Length: "
                    f"{len(body) if length is None else length}\r\nETag: {etag}\r\n"
                    f"Connection: close\r\n{extra}\r\n").encode() + body

        def partial(destination):
            run(destination, [response(original[:8192], length=len(original))], False)
            assert not destination.exists()
            assert Path(str(destination) + ".part").read_bytes() == original[:8192]

        destination = root / "resume.apk"
        partial(destination)
        requests = run(destination, [response(original[8192:], status=206,
            extra=f"Content-Range: bytes 8192-{len(original)-1}/{len(original)}\r\n")], True)
        assert "Range: bytes=8192-" in requests[0]
        assert 'If-Range: "v1"' in requests[0]
        assert destination.read_bytes() == original
        assert not Path(str(destination) + ".part").exists()

        destination = root / "changed.apk"
        partial(destination)
        requests = run(destination, [response(changed, etag='"v2"'),
                                     response(changed, etag='"v2"')], True, attempts=2)
        assert "Range:" in requests[0] and "Range:" not in requests[1]
        assert destination.read_bytes() == changed

        destination = root / "outage.apk"
        partial(destination)
        run(destination, [response(b"service unavailable", status=503)], False)
        assert Path(str(destination) + ".part").read_bytes() == original[:8192]
        run(destination, [response(original[8192:], status=206,
            extra=f"Content-Range: bytes 8192-{len(original)-1}/{len(original)}\r\n")], True)
        assert destination.read_bytes() == original

        destination = root / "wrong-range.apk"
        partial(destination)
        run(destination, [response(original[4096:], status=206,
            extra=f"Content-Range: bytes 4096-{len(original)-1}/{len(original)}\r\n"),
            response(original)], True, attempts=2)
        assert destination.read_bytes() == original

        destination = root / "expired-range.apk"
        partial(destination)
        run(destination, [response(b"", status=416), response(original)], True, attempts=2)
        assert destination.read_bytes() == original

        destination = root / "changed-etag.apk"
        partial(destination)
        run(destination, [response(changed[8192:], status=206, etag='"v2"',
            extra=f"Content-Range: bytes 8192-{len(changed)-1}/{len(changed)}\r\n"),
            response(changed, etag='"v2"')], True, attempts=2)
        assert destination.read_bytes() == changed

        destination = root / "redirect.apk"
        partial(destination)
        requests = run(destination, [response(b"redirect body", status=302,
            extra="Location: https://download.test/redirected\r\n"),
            response(original[8192:], status=206,
            extra=f"Content-Range: bytes 8192-{len(original)-1}/{len(original)}\r\n")], True)
        assert "GET /redirected " in requests[1]
        assert destination.read_bytes() == original

        destination = root / "new-url.apk"
        partial(destination)
        requests = run(destination, [response(changed, etag='"v2"')], True, path="/new-archive")
        assert "Range:" not in requests[0]
        assert destination.read_bytes() == changed

        destination = root / "weak-etag.apk"
        run(destination, [response(original[:8192], length=len(original), etag='W/"v1"')], False)
        requests = run(destination, [response(original)], True)
        assert "Range:" not in requests[0]
        assert destination.read_bytes() == original
        print("HTTPS download: restart, resume, validators, outages, redirects and ranges passed")


if __name__ == "__main__":
    main()
