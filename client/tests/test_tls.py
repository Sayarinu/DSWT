"""Exercise the actual DS TLS profile against fresh local ECDSA test peers."""

import hashlib
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from test_wire import messages, wire


PROBE = Path(sys.argv.pop(1)).resolve()
ACK = b"PLATINUMWT_ACK_V1\n"
RESPONSE = (
    b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
    b"Content-Length: 18\r\nConnection: close\r\n\r\n" + ACK
)

def http_response(body):
    return (b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
            + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)


def initial_record():
    record = bytearray(4096)
    struct.pack_into("<4sHHIIQ", record, 0, b"PWTJ", 1, 0, 4096, 0, 1)
    struct.pack_into("<HH", record, 1440, 1, 32)
    record[1444:1476] = os.urandom(32)
    record[4060:4092] = hashlib.sha256(record[:4060]).digest()
    record[4092:] = b"CMIT"
    return bytes(record)


class TlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.keys = []
        for name in ("current", "next", "untrusted"):
            key = cls.root / f"{name}.key"
            cert = cls.root / f"{name}.pem"
            subprocess.run([
                "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "1",
                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            public = subprocess.check_output(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"])
            cls.keys.append((key, cert, hashlib.sha256(public).hexdigest()))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.seed = self.root / "seed.bin"
        record = initial_record()
        self.seed.write_bytes(record * 2)

    def exchange(self, key=0, hostname="localhost", response=RESPONSE, fail_write=0, tls13=False, plaintext=False,
                 packet=None, stall_close=False):
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(7)
        port = server.getsockname()[1]
        received = []
        errors = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3 if tls13 else ssl.TLSVersion.TLSv1_2
        context.maximum_version = context.minimum_version
        context.set_ciphers("ECDHE-ECDSA-CHACHA20-POLY1305")
        context.load_cert_chain(self.keys[key][1], self.keys[key][0])

        def serve():
            try:
                raw, _ = server.accept()
                raw.settimeout(6)
                if plaintext:
                    with raw:
                        received.append(raw.recv(4096))
                        raw.sendall(RESPONSE)
                    return
                with context.wrap_socket(raw, server_side=True) as connection:
                    data = b""
                    while b"\r\n\r\n" not in data and len(data) < 2048:
                        part = connection.recv(512)
                        if not part:
                            break
                        data += part
                    if packet is not None and b"\r\n\r\n" in data:
                        header_size = data.index(b"\r\n\r\n") + 4
                        while len(data) - header_size < len(packet):
                            part = connection.recv(min(512, len(packet) - (len(data) - header_size)))
                            if not part:
                                break
                            data += part
                    received.append(data)
                    if data:
                        # Split inside the status line, header fields and response body.
                        for start in range(0, len(response), 7):
                            connection.sendall(response[start:start + 7])
                        if stall_close:
                            connection.recv(1)
            except (OSError, ssl.SSLError) as error:
                errors.append(type(error).__name__)
            finally:
                server.close()

        thread = threading.Thread(target=serve)
        thread.start()
        arguments = [
            str(PROBE), str(port), hostname, self.keys[0][2], self.keys[1][2], str(self.seed), str(fail_write),
        ]
        if packet is not None:
            request_file = self.root / "synthetic-request.bin"
            request_file.write_bytes(packet)
            request_file.chmod(0o600)
            arguments.append(str(request_file))
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=8)
        thread.join(8)
        self.assertFalse(thread.is_alive(), "peer did not shut down")
        if result.returncode in (0, 1):
            self.assertRegex(result.stderr, r"live=0 ")
        return result, received, errors

    def test_current_and_next_keys_are_authenticated(self):
        for key in (0, 1):
            with self.subTest(key=key):
                before = self.seed.read_bytes()
                result, received, _ = self.exchange(key=key)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(received[0].startswith(b"GET /v1/ping HTTP/1.1\r\n"))
                after = self.seed.read_bytes()
                self.assertEqual(after[:4096], after[4096:])
                self.assertNotEqual(before[1444:1476], after[1444:1476])
                self.assertGreater(struct.unpack_from("<Q", after, 16)[0], struct.unpack_from("<Q", before, 16)[0])
                peak = int(re.search(r"peak=(\d+)", result.stderr)[1])
                self.assertLess(peak, 65536)
                print(result.stderr.strip())

    def test_wrong_key_or_hostname_never_receives_http(self):
        for args in ({"key": 2}, {"hostname": "wrong.invalid"}):
            with self.subTest(args=args):
                result, received, _ = self.exchange(**args)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(received))

    def test_no_plaintext_or_version_fallback(self):
        for args in ({"plaintext": True}, {"tls13": True}):
            with self.subTest(args=args):
                result, received, _ = self.exchange(**args)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(b"GET /v1/ping" in data for data in received))

    def test_seed_failure_prevents_client_hello_and_recovers(self):
        for failure in (1, 2):
            with self.subTest(failure=failure):
                result, received, _ = self.exchange(fail_write=failure, plaintext=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(received))
                result, _, _ = self.exchange()
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_and_truncated_bodies_are_rejected(self):
        for response in (
            RESPONSE[:-1], RESPONSE.replace(b"200 OK", b"302 Found"),
            RESPONSE.replace(b"Content-Length: 18", b"Transfer-Encoding: chunked"),
            RESPONSE.replace(b"Content-Length: 18", b"Content-Length: 4097"),
            RESPONSE.replace(b"application/octet-stream", b"text/html"),
        ):
            with self.subTest(response=response[:40]):
                result, _, _ = self.exchange(response=response)
                self.assertNotEqual(result.returncode, 0)

    def test_all_post_operations_use_exact_frames_and_validate_replies(self):
        requests, replies = messages()
        for operation, packet in requests.items():
            with self.subTest(operation=operation):
                counter, highwater, body = replies[operation]
                reply = wire.encode_response(wire.Response(wire.Operation(operation), packet[8:24], wire.Result.OK,
                                                           counter, highwater, body))
                result, received, _ = self.exchange(packet=packet, response=http_response(reply))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "wire_result=1\n")
                headers, actual = received[0].split(b"\r\n\r\n", 1)
                self.assertEqual(actual, packet)
                self.assertEqual(headers.split(b"\r\n"), [
                    b"POST /wt/v1 HTTP/1.1", b"Host: localhost", b"Content-Type: application/octet-stream",
                    b"Content-Length: " + str(len(packet)).encode(), b"Connection: close"])
                processing = wire.encode_response(wire.Response(wire.Operation(operation), packet[8:24],
                                                                wire.Result.PROCESSING, 0, 0, b""))
                result, _, _ = self.exchange(packet=packet, response=http_response(processing))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "wire_result=2\n")

    def test_post_bindings_trailing_bytes_and_stalled_closure_fail_closed(self):
        requests, replies = messages()
        packet = requests[2]
        counter, highwater, body = replies[2]
        reply = wire.encode_response(wire.Response(wire.Operation.SUBMIT, packet[8:24], wire.Result.OK, counter, highwater, body))
        invalid = []
        for offset in (5, 8, 26, 44 + 8 + 34):
            changed = bytearray(reply)
            changed[offset] ^= 1
            invalid.append(http_response(changed))
        invalid += [http_response(reply[:-1]), http_response(reply)[:-1], http_response(reply) + b"x",
                    b"HTTP/1.1 503 Unavailable\r\nContent-Type: application/octet-stream\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"]
        for response in invalid:
            with self.subTest(response_size=len(response)):
                result, _, _ = self.exchange(packet=packet, response=response)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
        result, _, _ = self.exchange(packet=packet, response=http_response(reply), stall_close=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
