import socket

from server_fixture import ACK, ServerTestCase


class MalformedRequestTests(ServerTestCase):
    def test_bad_requests_do_not_break_server(self):
        port = self.start_server()
        for request in (
            b"invalid\r\n\r\n",
            b"GET / HTTP/1.0\r\nX: " + b"x" * 9000 + b"\r\n\r\n",
            b"POST /v1/ping HTTP/1.0\r\nContent-Length: 1000000\r\n\r\n",
        ):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
                connection.sendall(request)
                try:
                    response = connection.recv(4096)
                except ConnectionResetError:
                    response = b""
                self.assertNotIn(ACK, response)
        self.assertEqual(self.request(port)[:2], (200, ACK))
