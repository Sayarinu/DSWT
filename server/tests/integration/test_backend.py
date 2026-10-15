import socket
import time

from server_fixture import ACK, ServerTestCase


class BackendTests(ServerTestCase):
    def test_backend_failure_is_not_acknowledged(self):
        with socket.socket() as unavailable:
            unavailable.bind(("127.0.0.1", 0))
            port = self.start_server(
                WT_GRPC_TARGET=f"127.0.0.1:{unavailable.getsockname()[1]}"
            )
            started = time.monotonic()
            status, body, _ = self.request(port)
        self.assertEqual(status, 503)
        self.assertNotEqual(body, ACK)
        self.assertLess(time.monotonic() - started, 4)

    def test_backend_deadline(self):
        with socket.socket() as blackhole:
            blackhole.bind(("127.0.0.1", 0))
            blackhole.listen()
            port = self.start_server(
                WT_GRPC_TARGET=f"127.0.0.1:{blackhole.getsockname()[1]}"
            )
            started = time.monotonic()
            status, body, _ = self.request(port)
        self.assertEqual(status, 503)
        self.assertNotEqual(body, ACK)
        self.assertGreater(time.monotonic() - started, 1)
        self.assertLess(time.monotonic() - started, 4)
