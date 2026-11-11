"""Startup failure contracts and rejected requests against real Linux processes."""
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = os.path.abspath(sys.argv.pop(1))


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def exact(sock, size):
    result = b''
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise AssertionError('Unexpected EOF')
        result += part
    return result


class RuntimeContracts(unittest.TestCase):
    def test_invalid_startup_configuration_fails_cleanly(self):
        for key, value in [('FASTBID_TCP_PORT', '0'), ('FASTBID_TCP_PORT', '65536'),
                           ('FASTBID_TCP_PORT', '9000oops'), ('FASTBID_TCP_PORT', ''),
                           ('FASTBID_TCP_PORT', '-1'), ('FASTBID_UDP_MCAST_PORT', 'invalid'),
                           ('FASTBID_TCP_HOST', 'invalid'), ('FASTBID_UDP_MCAST_ADDR', 'invalid')]:
            with self.subTest(key=key, value=value):
                env = dict(os.environ, FASTBID_TCP_PORT=str(free_port()),
                           FASTBID_UDP_MCAST_PORT=str(free_port()), FASTBID_TCP_HOST='127.0.0.1',
                           FASTBID_UDP_MCAST_ADDR='127.0.0.1')
                env[key] = value
                result = subprocess.run([BINARY], env=env, capture_output=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertGreater(result.returncode, 0, result.stderr.decode())
                self.assertNotIn(b'AddressSanitizer', result.stderr)
                self.assertNotIn(b'runtime error:', result.stderr)

    def test_semantic_rejections_preserve_connection_and_ids(self):
        port = free_port()
        env = dict(os.environ, FASTBID_TCP_PORT=str(port), FASTBID_TCP_HOST='127.0.0.1',
                   FASTBID_UDP_MCAST_ADDR='127.0.0.1', FASTBID_UDP_MCAST_PORT=str(free_port()))
        with tempfile.TemporaryFile() as log:
            process = subprocess.Popen([BINARY], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        sock = socket.create_connection(('127.0.0.1', port), timeout=3)
                        break
                    except ConnectionRefusedError:
                        self.assertIsNone(process.poll())
                        if time.monotonic() > deadline:
                            raise
                        time.sleep(.02)
                with sock:
                    cases = [(1, 1, 1, 49999, 10, 1), (1, 1, 1, 150001, 10, 1),
                             (999, 1, 1, 100000, 10, 4), (1, 0, 1, 100000, 10, 5),
                             (1, 1, 0, 100000, 10, 5), (1, 1, 1, 100000, 0, 5)]
                    for i, (inst, side, kind, price, qty, reason) in enumerate(cases):
                        cid = 2**63 + i
                        sock.sendall(struct.pack('<HBBQIBBQIQ', 38, 1, 0, cid, inst, side, kind, price, qty, 0))
                        self.assertEqual(struct.unpack('<HBB', exact(sock, 4)), (28, 13, 0))
                        self.assertEqual(struct.unpack('<QQQ', exact(sock, 24)), (0, cid, reason))
                    for price in [50000, 150000]:
                        sock.sendall(struct.pack('<HBBQIBBQIQ', 38, 1, 0, price, 1, 1, 1, price, 1, 0))
                        self.assertEqual(struct.unpack('<HBB', exact(sock, 4)), (20, 10, 0))
                        oid, cid = struct.unpack('<QQ', exact(sock, 16))
                        self.assertEqual(oid, 1 if price == 50000 else 2)
                        self.assertEqual(cid, price)
                        sock.sendall(struct.pack('<HBBQI', 16, 2, 0, oid, 1))
                        self.assertEqual(struct.unpack('<HBB', exact(sock, 4)), (20, 12, 0))
                        exact(sock, 16)
                    sock.sendall(struct.pack('<HBBQI', 16, 2, 0, 999999, 1))
                    exact(sock, 4)
                    self.assertEqual(struct.unpack('<QQQ', exact(sock, 24)), (999999, 0, 3))
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=10), 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                log.seek(0)
                output = log.read()
                self.assertNotIn(b'AddressSanitizer', output)
                self.assertNotIn(b'runtime error:', output)


if __name__ == '__main__':
    unittest.main()
