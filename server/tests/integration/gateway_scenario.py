"""Independent v1 byte oracle over the real HTTPS gateway and private ledger.

Only the dedicated fixture grants synthetic legality/recovery coverage. No ROM,
save, production key or user Pokémon is read by this scenario.
"""

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import hashlib
import http.client
import os
import selectors
import socket
import ssl
import struct
import subprocess
import time
import uuid


REQUEST = struct.Struct('<4sBBH16s16s32sQ')
RESPONSE = struct.Struct('<4sBBH16sHHQQ')
SNAPSHOT = struct.Struct('<16sBB16s32s16s32s16sQ')
ZERO_ID = bytes(16)


@dataclass
class Client:
    save: bytes
    credential: bytes


class GatewayScenario:
    def __init__(self, test):
        self.test = test
        self.env = {key: value for key, value in dict(test.env, PGDATABASE=test.database).items()
                    if not key.startswith('WT_')}
        self.directory = test.root / 'gateway'
        self.directory.mkdir()
        self.certificate = self.directory / 'certificate.pem'
        self.key = self.directory / 'private-key.pem'
        result = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                                 '-nodes', '-keyout', str(self.key), '-out', str(self.certificate), '-days', '1',
                                 '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
                                capture_output=True, timeout=10)
        test.assertEqual(result.returncode, 0, result.stderr)
        os.chmod(self.key, 0o600)
        self.context = ssl.create_default_context(cafile=str(self.certificate))
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.maximum_version = ssl.TLSVersion.TLSv1_2
        self.context.set_ciphers('ECDHE-ECDSA-CHACHA20-POLY1305')
        self.credentials = []
        self.release = self.directory / 'release-validation'
        self.backend = self.start(test.gateway_fixture, PLATINUMWT_GATEWAY_FIXTURE_RELEASE=str(self.release))
        backend_port = int(self.mark(self.backend, b'TEST_BACKEND ').split()[-1])
        self.gateway, self.port = self.start_gateway(backend_port)

    def start(self, binary, **overrides):
        process = subprocess.Popen([str(binary)], env=dict(self.env, **overrides), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)

        def stop():
            process.terminate()
            try:
                out, err = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                out, err = process.communicate()
                self.test.fail('gateway fixture did not stop within its bound')
            for credential in self.credentials:
                self.test.assertNotIn(credential, out + err)

        self.test.addCleanup(stop)
        return process

    def mark(self, process, prefix, timeout=5):
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if selector.select(max(0, deadline - time.monotonic())):
                    line = process.stdout.readline()
                    self.test.assertTrue(line, 'fixture exited before its control marker')
                    if line.startswith(prefix):
                        return line
        self.test.fail('fixture control marker timed out')

    def start_gateway(self, backend_port, *, tls=True):
        settings = dict(WT_HTTP_ADDRESS='127.0.0.1', WT_HTTP_PORT='0', WT_GRPC_ADDRESS='127.0.0.1:0',
                        WT_TRADE_TARGET=f'127.0.0.1:{backend_port}')
        if tls:
            settings.update(WT_TLS_CERTIFICATE=str(self.certificate), WT_TLS_PRIVATE_KEY=str(self.key))
        process = self.start(self.test.server_binary, **settings)
        line = self.mark(process, b'HTTPS listening on ' if tls else b'HTTP listening on ')
        return process, int(line.rsplit(b':', 1)[1])

    def frame(self, operation, body, client=None, *, counter=0, request_id=None):
        return REQUEST.pack(b'WTDS', 1, operation, len(body), request_id or os.urandom(16),
                            client.save if client else ZERO_ID, client.credential if client else b'Q' * 32, counter) + body

    def post(self, packet, *, port=None, status=200):
        connection = http.client.HTTPSConnection('localhost', port or self.port, context=self.context, timeout=7)
        try:
            connection.request('POST', '/wt/v1', packet, {'Content-Type': 'application/octet-stream', 'Connection': 'close'})
            response = connection.getresponse()
            body = response.read()
            self.test.assertEqual(response.status, status)
            self.test.assertEqual(response.getheader('Cache-Control'), 'no-store')
            self.test.assertEqual(response.getheader('Connection'), 'close')
            self.test.assertEqual(int(response.getheader('Content-Length')), len(body))
            if status != 200:
                self.test.assertEqual(body, b'')
                return None
            self.test.assertEqual(response.getheader('Content-Type'), 'application/octet-stream')
        finally:
            connection.close()
        magic, version, operation, size, request, result, reserved, counter, highwater = RESPONSE.unpack_from(body)
        self.test.assertEqual((magic, version, operation, request, reserved), (b'WTDS', 1, packet[5], packet[8:24], 0))
        self.test.assertEqual(size, len(body) - RESPONSE.size)
        payload = body[RESPONSE.size:]
        if result != 1:
            self.test.assertEqual(payload, b'')
        return result, counter, highwater, payload

    def enroll(self, credential=b'Q' * 32, *, port=None):
        self.credentials.append(credential)
        packet = bytearray(self.frame(1, struct.pack('<IB', 1, 0)))
        packet[40:72] = credential
        first = self.post(packet, port=port)
        self.test.assertEqual(first[:3], (1, 1, 1))
        self.test.assertEqual(len(first[3]), 16)
        self.test.assertEqual(self.post(packet, port=port), first)
        return Client(first[3], credential)

    @staticmethod
    def pokemon(species=1):
        data = bytearray(136)
        struct.pack_into('<H', data, 6, species)
        struct.pack_into('<H', data, 8, species)
        return bytes(data)

    def submit(self, client, species=1, counter=1):
        data = self.pokemon(species)
        packet = self.frame(2, struct.pack('<Q', 1) + data + hashlib.sha256(data).digest(), client, counter=counter)
        return packet, self.post(packet)

    def raw(self, *, source='127.0.0.1'):
        transport = socket.create_connection(('127.0.0.1', self.port), timeout=7, source_address=(source, 0))
        return self.context.wrap_socket(transport, server_hostname='localhost')

    @staticmethod
    def http(packet, *, headers=None, version=b'1.1'):
        fields = headers if headers is not None else [b'Host: localhost', b'Connection: close',
                b'Content-Type: application/octet-stream', b'Content-Length: ' + str(len(packet)).encode()]
        return b'POST /wt/v1 HTTP/' + version + b'\r\n' + b'\r\n'.join(fields) + b'\r\n\r\n' + packet

    def reject_http(self, request):
        with self.raw() as connection:
            connection.sendall(request)
            try:
                response = http.client.HTTPResponse(connection)
                response.begin()
                body = response.read()
            except (OSError, http.client.HTTPException):
                return
            self.test.assertNotEqual(response.status, 200)
            self.test.assertNotIn(b'WTDS', body)

    def lifecycle(self):
        self.first, self.second = self.enroll(), self.enroll(b'R' * 32)
        self.test.assertNotEqual(self.first.save, self.second.save)
        self.test.assertEqual(self.post(self.frame(3, ZERO_ID, self.first))[3], bytes(138))
        first_packet, offered = self.submit(self.first)
        self.test.assertEqual(offered[:3], (1, 2, 2))
        self.first_offer = SNAPSHOT.unpack(offered[3][8:])
        with ThreadPoolExecutor(max_workers=1) as executor:
            pending = executor.submit(self.submit, self.second, 2)
            try:
                self.mark(self.backend, b'TEST_VALIDATION_STARTED')
            except AssertionError:
                if pending.done():
                    outcome = pending.result()[1]
                    self.test.fail(f'Submit ended before validation marker: result/counter/highwater={outcome[:3]}')
                raise
            before = time.monotonic()
            self.test.assertEqual(self.post(self.frame(3, ZERO_ID, self.first))[0], 1)
            self.test.assertLess(time.monotonic() - before, 1)
            second_packet, second_offer = pending.result(timeout=5)
        self.test.assertEqual(second_offer[:3], (1, 2, 2))
        self.second_offer = SNAPSHOT.unpack(second_offer[3][8:])
        operation = os.urandom(16)
        matched = subprocess.run([str(self.test.matcher_binary), '--match', operation.hex()], env=self.env,
                                 capture_output=True, timeout=10)
        self.test.assertEqual(matched.returncode, 0, matched.stdout + matched.stderr)
        incoming = self.post(self.frame(5, self.first_offer[0], self.first))
        self.test.assertEqual(incoming[:3], (1, 2, 3))
        self.test.assertEqual(incoming[3][138:], self.pokemon(2))
        snapshot = SNAPSHOT.unpack(incoming[3][:138])
        self.test.assertEqual(snapshot[1:3], (2, 1))
        self.test.assertEqual(snapshot[5], self.second_offer[3])
        # An old Submit reply remains its original immutable outcome after matching.
        self.test.assertEqual(self.post(first_packet)[3], offered[3])
        self.test.assertEqual(self.post(second_packet)[3], second_offer[3])
        self.test.assertEqual(self.post(self.frame(5, self.second_offer[0], self.first))[:3], (6, 2, 3))
        wrong = Client(self.first.save, b'S' * 32)
        self.credentials.append(wrong.credential)
        self.test.assertEqual(self.post(self.frame(3, ZERO_ID, wrong))[:3], (4, 0, 0))
        proof = self.first_offer[0] + snapshot[7] + snapshot[6] + snapshot[6] + struct.pack('<Q', 5)
        acknowledgement = self.frame(6, proof, self.first, counter=2)
        applied = self.post(acknowledgement)
        self.test.assertEqual(applied[:3], (1, 3, 4))
        self.test.assertEqual(SNAPSHOT.unpack(applied[3][8:])[1:3], (4, 1))
        self.test.assertEqual(self.post(acknowledgement), applied)
        self.test.assertEqual(self.post(self.frame(5, self.first_offer[0], self.first))[3][138:], self.pokemon(2))
        _, rejected = self.submit(self.first, 3, counter=3)
        self.test.assertEqual(rejected[:3], (8, 4, 5))
        page = self.post(self.frame(7, struct.pack('<QB', 0, 8), self.first))
        self.test.assertEqual(page[:3], (1, 4, 5))
        self.test.assertEqual(struct.unpack_from('<BBQ', page[3]), (5, 0, 5))
        self.test.assertEqual(len(page[3]), 10 + 5 * 106)
        for index in range(5):
            self.test.assertEqual(struct.unpack_from('<Q', page[3], 10 + index * 106)[0], index + 1)
        self.test.assertEqual(self.post(self.frame(4, self.second_offer[0], self.second, counter=2))[0], 5)
        returned_client = self.enroll(b'T' * 32)
        _, queued = self.submit(returned_client)
        returned_trade = SNAPSHOT.unpack(queued[3][8:])[0]
        cancelled = self.post(self.frame(4, returned_trade, returned_client, counter=2))
        self.test.assertEqual(cancelled[:3], (1, 3, 3))
        self.test.assertEqual(SNAPSHOT.unpack(cancelled[3][8:])[1:3], (3, 2))
        self.test.assertEqual(self.post(self.frame(5, returned_trade, returned_client))[3][138:], self.pokemon())
        # Lose a response after observing its local commit, then retry the same packet.
        lost = self.enroll(b'V' * 32)
        data = self.pokemon()
        packet = self.frame(2, struct.pack('<Q', 1) + data + hashlib.sha256(data).digest(), lost, counter=1)
        with self.raw() as connection:
            connection.sendall(self.http(packet))
            committed = False
            for _ in range(100):
                committed = self.test.sql("SELECT EXISTS(SELECT 1 FROM platinumwt.operations WHERE operation_id='" +
                                          str(uuid.UUID(bytes=packet[8:24])) + "')") == 't'
                if committed:
                    break
                time.sleep(0.01)
            self.test.assertTrue(committed)
        recovered = self.post(packet)
        self.test.assertEqual(recovered[:3], (1, 2, 2))
        self.test.assertEqual(self.post(packet), recovered)

    def malformed_and_backend_failures(self):
        valid = self.frame(3, ZERO_ID, self.first)
        packets = [valid[:79], b'BAD!' + valid[4:], valid[:4] + b'\x02' + valid[5:],
                   valid[:5] + b'\x08' + valid[6:], valid[:6] + b'\xff\xff' + valid[8:],
                   valid[:8] + bytes(16) + valid[24:], valid[:40] + bytes(32) + valid[72:],
                   valid[:72] + struct.pack('<Q', 1) + valid[80:], valid[:24] + ZERO_ID + valid[40:],
                   self.frame(7, struct.pack('<QB', 0, 0), self.first), self.frame(1, struct.pack('<IB', 1, 65)),
                   self.frame(5, ZERO_ID, self.first)]
        for packet in packets:
            self.post(packet, status=400)
        base = [b'Host: localhost', b'Connection: close', b'Content-Type: application/octet-stream',
                b'Content-Length: ' + str(len(valid)).encode()]
        for fields in (base + [base[-1]], base + [b'Transfer-Encoding: chunked'], base + [b'Content-Encoding: gzip'],
                       base + [b'Expect: 100-continue'], base + [b'Host: duplicate'],
                       [base[0], b'Connection: keep-alive', *base[2:]], base + [b'X-Fill: ' + b'x' * 2100]):
            self.reject_http(self.http(valid, headers=fields))
        self.reject_http(self.http(valid, version=b'1.0'))
        self.reject_http(self.http(bytes(4097)))
        self.reject_http(self.http(valid) + b'GET /v1/ping HTTP/1.1\r\nHost: localhost\r\n\r\n')
        data = self.pokemon()
        truncated = self.frame(2, struct.pack('<Q', 1) + data + hashlib.sha256(data).digest(), self.first, counter=4)
        with self.raw() as connection:
            connection.sendall(self.http(truncated)[:-1])
        time.sleep(0.05)
        self.test.assertEqual(self.test.sql("SELECT EXISTS(SELECT 1 FROM platinumwt.operations WHERE operation_id='" +
                                           str(uuid.UUID(bytes=truncated[8:24])) + "')"), 'f')
        for marker in (0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6):
            self.post(self.frame(3, bytes([marker]) * 16, self.first), status=503)

    def cancellation_and_timeouts(self):
        waiting = self.frame(3, b'\xF0' * 16, self.first)
        with ThreadPoolExecutor(max_workers=1) as executor:
            pending = executor.submit(self.post, waiting, status=503)
            self.mark(self.backend, b'TEST_WAIT_STARTED')
            before = time.monotonic()
            self.test.assertEqual(self.post(self.frame(3, ZERO_ID, self.first))[0], 1)
            self.test.assertLess(time.monotonic() - before, 1)
            pending.result(timeout=6)
            self.mark(self.backend, b'TEST_WAIT_CANCELLED')
        with self.raw() as connection:
            connection.sendall(self.http(waiting))
            self.mark(self.backend, b'TEST_WAIT_STARTED')
        self.mark(self.backend, b'TEST_WAIT_CANCELLED', timeout=2)
        # A stalled body cannot hold the event loop or reach a mutation handler.
        with self.raw() as connection:
            connection.sendall(b'POST /wt/v1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
                               b'Content-Type: application/octet-stream\r\nContent-Length: 256\r\n\r\n')
            started = time.monotonic()
            self.test.assertEqual(self.post(self.frame(3, ZERO_ID, self.first))[0], 1)
            try:
                received = connection.recv(1024)
                self.test.assertEqual(received, b'')
            except (ConnectionResetError, ssl.SSLError):
                pass
            self.test.assertLess(time.monotonic() - started, 6.5)
        # Progress resets the idle limit, but cannot extend the total 15 seconds.
        with self.raw() as connection:
            connection.sendall(b'POST /wt/v1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
                               b'Content-Type: application/octet-stream\r\nContent-Length: 256\r\n\r\n')
            connection.settimeout(0.25)
            started = time.monotonic()
            closed = False
            for _ in range(6):
                time.sleep(3)
                try:
                    connection.sendall(b'x')
                    closed = connection.recv(1) == b''
                except socket.timeout:
                    continue
                except (OSError, ssl.SSLError):
                    closed = True
                if closed:
                    break
            elapsed = time.monotonic() - started
            self.test.assertTrue(closed)
            self.test.assertGreater(elapsed, 14)
            self.test.assertLess(elapsed, 17)

    def closed_runtime_and_plaintext(self):
        backend = self.start(self.test.backend_binary, WT_BACKEND_ADDRESS='127.0.0.1:0',
                             PLATINUMWT_DATABASE_URL=f'dbname={self.test.database}', WT_LEGALITY_POLICY='synthetic-policy')
        port = int(self.mark(backend, b'Trade backend listening on loopback port ').split()[-1])
        _, gateway_port = self.start_gateway(port)
        packet = self.frame(1, struct.pack('<IB', 1, 0))
        self.test.assertEqual(self.post(packet, port=gateway_port), (2, 0, 0, b''))
        probe = subprocess.run([str(self.test.gateway_fixture), '--probe-worker', f'127.0.0.1:{port}'],
                               env=self.env, capture_output=True, timeout=5)
        self.test.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
        _, plaintext_port = self.start_gateway(port, tls=False)
        connection = http.client.HTTPConnection('127.0.0.1', plaintext_port, timeout=3)
        try:
            connection.request('POST', '/wt/v1', packet, {'Content-Type': 'application/octet-stream', 'Connection': 'close'})
            response = connection.getresponse()
            self.test.assertEqual(response.status, 403)
            self.test.assertEqual(response.read(), b'')
        finally:
            connection.close()

    def overload_and_recovery(self):
        # Fill both running and queued submission slots while recovering a
        # different accepted deposit on workers that submissions cannot use.
        owners = [self.enroll(os.urandom(32)) for _ in range(9)]
        packets = [self.frame(2, struct.pack('<Q', 1) + self.pokemon(4) + hashlib.sha256(self.pokemon(4)).digest(),
                              owner, counter=1) for owner in owners]
        returning = self.enroll(os.urandom(32))
        _, offered = self.submit(returning)
        trade = SNAPSHOT.unpack(offered[3][8:])[0]
        cancelled = self.post(self.frame(4, trade, returning, counter=2))
        snapshot = SNAPSHOT.unpack(cancelled[3][8:])
        original = self.pokemon()
        with ThreadPoolExecutor(max_workers=4) as executor:
            active = [executor.submit(self.post, packet) for packet in packets[:4]]
            queued = []
            try:
                for _ in range(4):
                    self.mark(self.backend, b'TEST_VALIDATION_HELD')
                for packet in packets[4:8]:
                    connection = self.raw()
                    queued.append(connection)
                    connection.sendall(self.http(packet))
                before = time.monotonic()
                collected = self.post(self.frame(5, trade, returning))
                self.test.assertEqual(collected[3][138:], original)
                proof = trade + snapshot[7] + hashlib.sha256(original).digest() + hashlib.sha256(b'applied recovery').digest()
                acknowledgement = self.frame(6, proof + struct.pack('<Q', 9), returning, counter=3)
                acknowledged = self.post(acknowledgement)
                self.test.assertEqual(acknowledged[:3], (1, 4, 4))
                self.test.assertEqual(self.post(self.frame(7, struct.pack('<QB', 0, 8), returning))[0], 1)
                self.test.assertEqual(self.post(acknowledgement), acknowledged)
                self.test.assertLess(time.monotonic() - before, 1.5)
                self.post(packets[8], status=503)
                self.test.assertEqual(self.test.sql("SELECT count(*) FROM platinumwt.operations WHERE operation_id='" +
                                                   str(uuid.UUID(bytes=packets[8][8:24])) + "'"), '0')
            finally:
                self.release.touch()
                for connection in queued:
                    connection.close() # Ambiguous responses must be recovered by the same operation.
            for future in active:
                self.test.assertEqual(future.result(timeout=5)[0], 1)
        # Queued calls may have committed or been cancelled. Exact retries yield
        # one owned offer either way, with no duplicate payload or operation.
        for packet in packets[4:]:
            self.test.assertEqual(self.post(packet)[0], 1)
        for packet, owner in zip(packets, owners):
            self.test.assertEqual(self.post(packet)[0], 1)
            self.test.assertEqual(self.test.sql("SELECT count(*) FROM platinumwt.operations WHERE save_id='" +
                                               str(uuid.UUID(bytes=owner.save)) + "'"), '1')

        # A validator outage leaves the mutation unconsumed and accepted assets
        # collectable; the exact pending packet succeeds after validation returns.
        outage = self.directory / 'release-validation.outage'
        outage.touch()
        try:
            packet, unavailable = self.submit(owners[8], counter=2) # Existing pending deposit wins capacity before validation.
            self.test.assertEqual(unavailable[0], 7)
            waiting = self.enroll(os.urandom(32))
            packet, unavailable = self.submit(waiting)
            self.test.assertEqual(unavailable[:3], (11, 1, 1))
            self.test.assertEqual(self.post(self.frame(5, trade, returning))[3][138:], original)
            self.test.assertEqual(self.test.sql("SELECT count(*) FROM platinumwt.operations WHERE save_id='" +
                                               str(uuid.UUID(bytes=waiting.save)) + "'"), '0')
        finally:
            outage.unlink()
        self.test.assertEqual(self.post(packet)[0], 1)

        # Database loss produces only a retryable empty HTTP failure. Retained
        # outcomes and delivered bytes survive an immediate stop/restart.
        self.test.stop()
        try:
            self.post(acknowledgement, status=503)
            self.post(self.frame(5, trade, returning), status=503)
        finally:
            self.test.start()
        self.test.assertEqual(self.post(acknowledgement), acknowledged)
        self.test.assertEqual(self.post(self.frame(5, trade, returning))[3][138:], original)

    def connection_exhaustion(self):
        # No TLS handshake is required to reach the per-network connection cap.
        # Another direct peer retains capacity under the same global cap.
        stalled = []
        try:
            for _ in range(64):
                stalled.append(socket.create_connection(('127.0.0.1', self.port), timeout=2))
            with socket.create_connection(('127.0.0.1', self.port), timeout=2) as extra:
                self.test.assertEqual(extra.recv(1), b'')
            with self.raw(source='127.0.0.2') as connection:
                packet = self.frame(3, ZERO_ID, self.first)
                connection.sendall(self.http(packet))
                response = http.client.HTTPResponse(connection)
                response.begin()
                self.test.assertEqual(response.status, 200)
                self.test.assertEqual(RESPONSE.unpack_from(response.read())[5], 1)
        finally:
            for connection in stalled:
                connection.close()


def exercise(test):
    scenario = GatewayScenario(test)
    scenario.lifecycle()
    scenario.malformed_and_backend_failures()
    scenario.cancellation_and_timeouts()
    scenario.closed_runtime_and_plaintext()
    scenario.overload_and_recovery()
    scenario.connection_exhaustion()
