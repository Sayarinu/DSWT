import socket

from server_fixture import ACK, ServerTestCase


class HttpGatewayTests(ServerTestCase):
    def test_acknowledgement_and_routes(self):
        port = self.start_server()
        for _ in range(3):
            status, body, headers = self.request(port)
            self.assertEqual((status, body), (200, ACK))
            self.assertEqual(int(headers["Content-Length"]), len(ACK))
            self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual(self.request(port, path="/unknown")[0], 404)
        self.assertEqual(self.request(port, method="POST")[0], 405)

    def test_http_10_without_chunking(self):
        port = self.start_server()
        with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
            connection.sendall(b"GET /v1/ping HTTP/1.0\r\n\r\n")
            data = b""
            while chunk := connection.recv(4096):
                data += chunk
        self.assertTrue(data.startswith(b"HTTP/1.0 200"))
        self.assertNotIn(b"Transfer-Encoding", data)
        self.assertEqual(data.split(b"\r\n\r\n", 1)[1], ACK)
