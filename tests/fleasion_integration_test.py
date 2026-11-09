"""Exercise real CA composition, native path mapping, and HTTPS CONNECT routing."""
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading


def command(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def main():
    probe, openssl = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="mocktail-fleasion-test-") as directory:
        root = Path(directory)
        for name in ("public", "fleasion", "rotated"):
            command(openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                    "-subj", f"/CN={name}", "-addext", "basicConstraints=critical,CA:TRUE",
                    "-keyout", str(root / f"{name}.key"), "-out", str(root / f"{name}.crt"))
        command(openssl, "req", "-newkey", "rsa:2048", "-nodes", "-subj",
                "/CN=assetdelivery.roblox.com", "-keyout", str(root / "leaf.key"),
                "-out", str(root / "leaf.csr"))
        (root / "leaf.ext").write_text("subjectAltName=DNS:assetdelivery.roblox.com\nbasicConstraints=CA:FALSE\n")
        command(openssl, "x509", "-req", "-in", str(root / "leaf.csr"), "-CA",
                str(root / "fleasion.crt"), "-CAkey", str(root / "fleasion.key"),
                "-CAcreateserial", "-days", "1", "-extfile", str(root / "leaf.ext"),
                "-out", str(root / "leaf.crt"))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root / "leaf.crt", root / "leaf.key")
        intercepted = []
        failures = []
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(5)
        port = listener.getsockname()[1]

        def serve():
            try:
                # Correct CA succeeds; the unrelated CA must fail TLS verification.
                for attempt in range(2):
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(5)
                        connect = b""
                        while not connect.endswith(b"\r\n\r\n"):
                            data = connection.recv(4096)
                            if not data:
                                raise AssertionError("proxy connection closed before CONNECT")
                            connect += data
                        assert connect.startswith(b"CONNECT assetdelivery.roblox.com:443 "), connect
                        connection.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                        try:
                            with context.wrap_socket(connection, server_side=True) as tls:
                                request = tls.recv(8192)
                                assert request.startswith(b"GET /replacement "), request
                                intercepted.append(request)
                                body = b"fleasion-replaced-asset"
                                tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: " +
                                            str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
                        except (ssl.SSLError, ConnectionError):
                            if attempt == 0:
                                raise
                            # The wrong CA may close with either a TLS alert or
                            # a TCP reset before the server finishes its handshake.
            except BaseException as error:
                failures.append(error)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MOCKTAIL_", "ROBLOX_"))}
        for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
            env.pop(key, None)
        env.update(HOME=str(root), XDG_CONFIG_HOME=str(root / "config"),
                   MOCKTAIL_CACHE_ROOT=str(root / "cache"), NO_PROXY="*", no_proxy="*")
        certificate = root / "config/Fleasion/proxy_ca/ca.crt"
        certificate.parent.mkdir(parents=True)
        original = (root / "fleasion.crt").read_bytes()
        certificate.write_bytes(original)
        config = root / "config.yaml"

        def configure(extra="", enabled="true", mode="env"):
            config.write_text(f"version: 1\nnetwork:\n  ca_bundle: {root / 'public.crt'}\n"
                              f"integrations:\n  fleasion:\n    enabled: {enabled}\n"
                              f"    proxy_mode: {mode}\n    proxy_port: {port}\n{extra}")

        def run(url="prepare", success=True, environment=None):
            result = subprocess.run([probe, str(config), url], env=environment or env,
                                    capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == success, result.stdout + result.stderr
            return result

        configure()
        response = run("https://assetdelivery.roblox.com/replacement")
        assert "fleasion-replaced-asset" in response.stdout
        assert certificate.read_bytes() == original
        generated = root / "cache/fleasion/cacert.pem"
        assert generated.stat().st_mode & 0o777 == 0o600
        command(openssl, "verify", "-CAfile", str(generated), str(root / "public.crt"))
        first_bundle = generated.read_bytes()
        run()  # Repeated startup must not duplicate trust roots.
        assert generated.read_bytes() == first_bundle
        certificate.write_bytes((root / "rotated.crt").read_bytes())
        response = run("https://assetdelivery.roblox.com/replacement", success=False)
        assert response.returncode == 10, response.stderr
        thread.join(timeout=7)
        listener.close()
        assert not thread.is_alive() and not failures, failures
        assert len(intercepted) == 1
        assert original not in generated.read_bytes()
        command(openssl, "verify", "-CAfile", str(generated), str(root / "rotated.crt"))
        # Re-exec must compose from the original public roots, not old proxy CAs.
        inherited = dict(env, MOCKTAIL_CA_BUNDLE=str(generated),
                         MOCKTAIL_FLEASION_GENERATED_BUNDLE=str(generated),
                         MOCKTAIL_FLEASION_BASE_CA_BUNDLE=str(root / "public.crt"))
        certificate.write_bytes(original)
        run(environment=inherited)
        assert generated.read_bytes() == first_bundle
        for bad in (b"not a certificate", (root / "leaf.crt").read_bytes()):
            certificate.write_bytes(bad)
            run(success=False)
            assert generated.read_bytes() == first_bundle
        certificate.unlink()
        run(success=False)
        configure(enabled="false")
        run()  # Disabled integration does not require Fleasion.
        certificate.write_bytes(original)
        configure(mode="hosts")
        run()
        configure(mode="invalid")
        run(success=False)
        configure(extra="    ca_certificate: relative.crt\n")
        run(success=False)
        print("Fleasion integration: proxy replacement, TLS rejection, CA rotation, "
              "native aliases, and configuration checks passed")


if __name__ == "__main__":
    main()
