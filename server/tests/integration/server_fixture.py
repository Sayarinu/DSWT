"""Shared process lifecycle and HTTP helpers for integration tests."""

import http.client
import os
import selectors
import subprocess
import unittest

ACK = b"PLATINUMWT_ACK_V1\n"


class ServerTestCase(unittest.TestCase):
    server_binary = None

    def start_server(self, **overrides):
        env = dict(
            os.environ,
            WT_HTTP_ADDRESS="127.0.0.1",
            WT_HTTP_PORT="0",
            WT_GRPC_ADDRESS="127.0.0.1:0",
        )
        env.pop("WT_GRPC_TARGET", None)
        env.pop("WT_METRICS_PORT", None)
        env.update(overrides)
        process = subprocess.Popen(
            [self.server_binary],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        def stop():
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                self.fail("server did not shut down")

        self.addCleanup(stop)
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            self.assertTrue(selector.select(timeout=10), "server did not start")
            line = process.stdout.readline()
        self.assertTrue(line.startswith("HTTP listening on "), line)
        return int(line.rsplit(":", 1)[1])

    def request(self, port, method="GET", path="/v1/ping"):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        self.addCleanup(connection.close)
        connection.request(method, path)
        response = connection.getresponse()
        return response.status, response.read(), dict(response.getheaders())
