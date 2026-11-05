import socket
import http.client

from server_fixture import ACK, ServerTestCase


class HttpGatewayTests(ServerTestCase):
    def test_metrics_use_a_separate_listener_and_are_absent_from_game_routes(self):
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            metrics_port = reserve.getsockname()[1]
        game_port = self.start_server(WT_METRICS_PORT=str(metrics_port))
        connection = http.client.HTTPConnection("127.0.0.1", metrics_port, timeout=4)
        self.addCleanup(connection.close)
        connection.request("GET", "/metrics")
        response = connection.getresponse()
        text = response.read().decode()
        self.assertEqual(response.status, 200)
        self.assertIn("version=0.0.4", response.getheader("Content-Type"))
        self.assertIn('service="gateway"', text)
        self.assertNotIn("dswt_database_up", text)
        self.assertEqual(self.request(game_port, path="/metrics")[0], 404)
        connection = http.client.HTTPConnection("127.0.0.1", metrics_port, timeout=4)
        self.addCleanup(connection.close)
        connection.request("POST", "/metrics")
        self.assertEqual(connection.getresponse().status, 405)

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
