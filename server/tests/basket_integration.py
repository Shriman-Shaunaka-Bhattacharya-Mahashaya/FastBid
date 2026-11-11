"""Real TCP basket registration, resynchronization, isolation and whole-unit fills."""
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def exact(sock, count):
    result = b''
    while len(result) < count:
        data = sock.recv(count - len(result))
        assert data, 'Unexpected disconnect'
        result += data
    return result


def response(sock):
    header = exact(sock, 4)
    length, kind, reserved = struct.unpack('<HBB', header)
    assert length in (20, 28, 36, 236) and reserved == 0
    return kind, header + exact(sock, length - 4)


def register(sock, request, name, legs):
    frame = struct.pack('<HBBQHH48s', 192, 4, 0, request, len(legs), 0, name.encode())
    frame += b''.join(struct.pack('<II', *leg) for leg in legs)
    frame += bytes(192 - len(frame))
    # Header and payload fragmentation must survive the control parser.
    sock.sendall(frame[:3])
    time.sleep(.002)
    sock.sendall(frame[3:])
    kind, packet = response(sock)
    assert kind == 14 and struct.unpack_from('<Q', packet, 4)[0] == request
    return struct.unpack_from('<Q', packet, 12)[0], packet


def new(sock, instrument, side, quantity, price=500000):
    sock.sendall(struct.pack('<HBBQIBBQIQ', 38, 1, 0, instrument, instrument, side, 1, price, quantity, 0))
    kind, packet = response(sock)
    assert kind == 10
    return struct.unpack_from('<Q', packet, 4)[0]


def cancel(sock, instrument, oid):
    sock.sendall(struct.pack('<HBBQI', 16, 2, 0, oid, instrument))
    return response(sock)


tcp = port()
env = dict(os.environ, FASTBID_TCP_HOST='127.0.0.1', FASTBID_TCP_PORT=str(tcp),
           FASTBID_UDP_MCAST_ADDR='127.0.0.1', FASTBID_UDP_MCAST_PORT=str(port()))
clients = []
with tempfile.TemporaryFile() as log:
    process = subprocess.Popen([sys.argv[1]], env=env, stdout=log, stderr=log)
    try:
        deadline = time.monotonic() + 20
        while True:
            try:
                a = socket.create_connection(('127.0.0.1', tcp), timeout=5)
                clients.append(a)
                break
            except ConnectionRefusedError:
                assert process.poll() is None
                if time.monotonic() > deadline:
                    raise
                time.sleep(.02)
        b = socket.create_connection(('127.0.0.1', tcp), timeout=5)
        clients.append(b)
        reason, _ = register(a, 1, 'Invalid', [(1, 0), (2, 3)])
        assert reason == 9
        reason, packet = register(a, 2, 'Fruit Mix', [(1, 2), (2, 3)])
        assert reason == 0
        basket, tick = struct.unpack_from('<II', packet, 20)
        assert basket == 0x80000000 and tick == 5
        assert struct.unpack_from('<QQQ', packet, 28) == (250000, 750000, 500000)
        reason, duplicate = register(b, 3, 'Another Name', [(2, 3), (1, 2)])
        assert reason == 0 and duplicate[20:] == packet[20:]
        # A new control connection rebuilds authoritative metadata without recreating a basket.
        b.sendall(struct.pack('<HBBQ', 12, 5, 0, 4))
        kind, copied = response(b)
        assert kind == 14 and copied[20:] == packet[20:]
        assert response(b)[0] == 15
        stock = new(b, 1, 2, 100, price=100000)
        buyer = new(a, basket, 1, 3)
        seller = new(b, basket, 2, 1)
        kind, sell_fill = response(b)
        assert kind == 11 and struct.unpack_from('<Q', sell_fill, 4)[0] == seller
        kind, buy_fill = response(a)
        assert kind == 11 and struct.unpack_from('<Q', buy_fill, 4)[0] == buyer
        assert struct.unpack_from('<II', buy_fill, 28) == (1, 2)
        assert struct.unpack_from('<Q', buy_fill, 20)[0] == 500000
        kind, bad_cancel = cancel(a, 1, buyer)
        assert kind == 13 and struct.unpack_from('<Q', bad_cancel, 20)[0] == 3
        assert cancel(a, basket, buyer)[0] == 12
        assert cancel(b, 1, stock)[0] == 12  # basket never consumed this base liquidity
        # Registry capacity is bounded, and an existing recipe still works at capacity.
        for i in range(7):
            assert register(a, 10 + i, f'Mix {i}', [(1, i + 3), (2, 1)])[0] == 0
        assert register(a, 30, 'Ninth', [(1, 30), (2, 1)])[0] == 10
        assert register(a, 31, 'Existing', [(1, 2), (2, 3)])[0] == 0
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=15) == 0
        print('Basket control integration passed: registration, canonical reuse, sync, whole fills, isolation, capacity')
    finally:
        for client in clients:
            client.close()
        if process.poll() is None:
            process.kill()
            process.wait()
        log.seek(0)
        output = log.read()
        assert b'AddressSanitizer' not in output and b'runtime error:' not in output, output
