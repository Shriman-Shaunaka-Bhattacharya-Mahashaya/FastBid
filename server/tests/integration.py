#!/usr/bin/env python3
"""Real process/socket regression: malformed frames, fragmentation, correlation,
ownership, descriptor reuse, pipelining/backpressure, market snapshots, shutdown."""
import os, signal, socket, struct, subprocess, sys, time

def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(('127.0.0.1', 0)); return s.getsockname()[1]

port, udp_port = free_port(), free_port(socket.SOCK_DGRAM)
env = dict(os.environ, FASTBID_TCP_PORT=str(port), FASTBID_UDP_MCAST_PORT=str(udp_port), FASTBID_UDP_MCAST_ADDR='127.0.0.1')
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(('127.0.0.1', udp_port)); udp.settimeout(5)
log = open('integration-server.log', 'w+')
p = subprocess.Popen([sys.argv[1]], env=env, stdout=log, stderr=log)
connections = []
def connect():
    s = socket.create_connection(('127.0.0.1', port), timeout=5); connections.append(s); return s

def new(cid, side=1, price=100000, qty=10):
    return struct.pack('<HBBQIBBQIQ', 38, 1, 0, cid, 1, side, 1, price, qty, 0)

def cancel(oid): return struct.pack('<HBBQI', 16, 2, 0, oid, 1)
def exact(s, n):
    b = b''
    while len(b) < n:
        chunk = s.recv(n-len(b)); assert chunk, 'Unexpected disconnect'; b += chunk
    return b

def response(s):
    h = exact(s, 4); n, kind, reserved = struct.unpack('<HBB', h)
    assert n in (20, 28, 36) and reserved == 0
    return kind, exact(s, n-4)

try:
    deadline = time.monotonic()+15
    while True:
        try: a = connect(); break
        except ConnectionRefusedError:
            assert p.poll() is None, 'Server exited at startup'
            if time.monotonic()>deadline: raise
            time.sleep(.02)
    packet = new(42)
    for byte in packet: a.sendall(bytes([byte]))
    kind, payload = response(a); assert kind == 10
    oid, cid = struct.unpack('<QQ', payload); assert cid == 42
    b = connect(); b.sendall(cancel(oid)); kind, payload = response(b)
    assert kind == 13 and struct.unpack('<QQQ', payload)[2] == 7
    b.sendall(new(43, 2, qty=10)); assert response(b)[0] == 10
    assert response(b)[0] == 11 and response(a)[0] == 11
    a.sendall(new(44, qty=1)); kind, payload = response(a); oid = struct.unpack('<QQ', payload)[0]
    a.sendall(cancel(oid)); assert response(a)[0] == 12
    # Reused OS descriptors must not grant cancellation ownership to a new session.
    a.sendall(new(45, price=90000)); _, payload = response(a); old = struct.unpack('<QQ', payload)[0]; a.close()
    c = connect(); c.sendall(cancel(old)); kind, payload = response(c)
    assert kind == 13 and struct.unpack('<QQQ', payload)[2] == 7
    for length, kind, reserved in [(0,1,0),(3,1,0),(65535,1,0),(38,99,0),(38,1,1),(16,3,0)]:
        bad = connect(); bad.sendall(struct.pack('<HBB', length, kind, reserved)); assert bad.recv(1) == b''; bad.close()
    c.sendall(new(46, qty=0)); assert response(c)[0] == 13
    # Many coalesced messages and fragmented responses maintain exact order/correlation.
    count = 2000
    c.sendall(b''.join(new(1000+i, price=80000, qty=1) for i in range(count)))
    ids = []
    for i in range(count):
        kind, payload = response(c); assert kind == 10
        oid, cid = struct.unpack('<QQ', payload); assert cid == 1000+i; ids.append(oid)
    c.sendall(b''.join(cancel(oid) for oid in ids))
    for _ in ids: assert response(c)[0] == 12
    # Snapshot includes real remaining orphan order; padding-free packet is versioned.
    deadline = time.monotonic()+5
    found = False
    while time.monotonic()<deadline:
        msg = udp.recv(2048)
        if len(msg) == 392 and struct.unpack_from('<IHH', msg) == (0x31444246,2,2):
            bids = struct.unpack_from('<I',msg,24)[0]
            if msg[16:20] == struct.pack('<I', 1) and bids and struct.unpack_from('<QQ',msg,32) == (90000,10): found = True; break
    assert found, 'No matching real snapshot'
    p.send_signal(signal.SIGTERM); assert p.wait(timeout=10) == 0
    print('Socket integration passed: framing, fills, ownership, IDs, 2000 pipelined requests, snapshots, shutdown')
finally:
    for s in connections: s.close()
    udp.close()
    if p.poll() is None: p.kill(); p.wait()
    log.flush(); log.seek(0); output = log.read(); log.close()
    assert 'AddressSanitizer' not in output and 'runtime error:' not in output, output
