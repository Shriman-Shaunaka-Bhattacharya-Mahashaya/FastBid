import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import net from 'node:net';
import dgram from 'node:dgram';
import { WebSocket } from 'ws';
import { fileURLToPath } from 'node:url';

const cwd = fileURLToPath(new URL('../', import.meta.url));
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate, message, timeout = 8000) {
    const deadline = Date.now() + timeout;
    while (!predicate()) {
        assert(Date.now() < deadline, message);
        await delay(10);
    }
}
async function freeTCP() {
    const server = net.createServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
    const port = server.address().port; await new Promise(resolve => server.close(resolve)); return port;
}
async function freeUDP() {
    const socket = dgram.createSocket('udp4'); socket.bind(0, '127.0.0.1'); await once(socket, 'listening');
    const port = socket.address().port; socket.close(); return port;
}
function snapshot(sequence, count = 1, instrument = 1) {
    const b = Buffer.alloc(392);
    b.writeUInt32LE(0x31444246); b.writeUInt16LE(2, 4); b.writeUInt16LE(2, 6);
    b.writeBigUInt64LE(BigInt(sequence), 8); b.writeUInt32LE(instrument, 16); b.writeUInt32LE(count, 24);
    b.writeBigUInt64LE(100001n, 32); b.writeBigUInt64LE(17n, 40);
    b.writeBigUInt64LE(100n, 352); b.writeBigUInt64LE(200n, 360); b.writeBigUInt64LE(300n, 368);
    b.writeBigUInt64LE(10n, 376); return b;
}

test('bridge contracts with a controlled engine and UDP source', { timeout: 45000 }, async t => {
    const tcpPort = await freeTCP(), wsPort = await freeTCP(), udpPort = await freeUDP();
    const frames = [], sockets = new Set(), browsers = [];
    let corrupt = false;
    const fake = net.createServer(socket => {
        sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket));
        let rx = Buffer.alloc(0);
        socket.on('data', data => {
            rx = Buffer.concat([rx, data]);
            while (rx.length >= 4 && rx.length >= rx.readUInt16LE(0)) {
                const length = rx.readUInt16LE(0); assert(length >= 4);
                const frame = Buffer.from(rx.subarray(0, length)); frames.push(frame); rx = rx.subarray(length);
                if (corrupt) { socket.write(Buffer.from([0, 0, 99, 0])); continue; }
                if (frame[2] !== 1) continue;
                const ack = Buffer.alloc(20); ack.writeUInt16LE(20); ack[2] = 10;
                ack.writeBigUInt64LE(9007199254740993n, 4); ack.writeBigUInt64LE(frame.readBigUInt64LE(4), 12);
                // Deliberately fragment a response across independent writes.
                socket.write(ack.subarray(0, 3)); setTimeout(() => { if (!socket.destroyed) socket.write(ack.subarray(3)); }, 5);
            }
        });
    });
    fake.listen(tcpPort, '127.0.0.1'); await once(fake, 'listening');
    const child = spawn(process.execPath, ['ws_bridge.js'], { cwd, env: { ...process.env,
        FASTBID_TCP_HOST: '127.0.0.1', FASTBID_TCP_PORT: String(tcpPort), FASTBID_WS_HOST: '127.0.0.1',
        FASTBID_WS_PORT: String(wsPort), FASTBID_UDP_MCAST_PORT: String(udpPort),
        FASTBID_UDP_MCAST_ADDR: '239.255.0.1', FASTBID_ALLOWED_ORIGINS: 'http://localhost:5173' },
        stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', b => { output += b; }); child.stderr.on('data', b => { output += b; });
    const udp = dgram.createSocket('udp4');
    t.after(async () => {
        for (const ws of browsers) ws.terminate();
        for (const socket of sockets) socket.destroy();
        udp.close(); await new Promise(resolve => fake.close(resolve));
        if (child.exitCode === null) {
            const closed = once(child, 'exit'); child.kill('SIGTERM');
            const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
            await closed; clearTimeout(timer);
        }
    });
    await until(() => output.includes('[Bridge] Listening'), 'Bridge failed to listen: ' + output);
    const base = `http://127.0.0.1:${wsPort}`;
    async function connect() {
        const ws = new WebSocket(`ws://127.0.0.1:${wsPort}`, { origin: 'http://localhost:5173' });
        browsers.push(ws); ws.messages = []; ws.on('error', () => {});
        ws.on('message', bytes => ws.messages.push(JSON.parse(bytes)));
        await until(() => ws.messages.some(m => m.type === 'ready'), 'No engine readiness'); return ws;
    }
    async function sendUDP(packet) {
        await new Promise((resolve, reject) => udp.send(packet, udpPort, '127.0.0.1', err => err ? reject(err) : resolve()));
    }
    const a = await connect();

    await t.test('HTTP methods, types, HEAD, missing assets and encoded traversal', async () => {
        assert.equal((await fetch(`${base}/healthz`)).status, 503);
        const page = await fetch(base); assert.equal(page.status, 200);
        assert.match(page.headers.get('content-type'), /text\/html/);
        const html = await page.text(); const asset = html.match(/src="([^"]+\.js)"/)[1];
        const script = await fetch(new URL(asset, base)); assert.equal(script.status, 200);
        assert.equal(script.headers.get('x-content-type-options'), 'nosniff');
        const head = await fetch(base, { method: 'HEAD' }); assert.equal(head.status, 200); assert.equal(await head.text(), '');
        assert.equal((await fetch(base, { method: 'POST' })).status, 405);
        for (const route of ['/missing', '/ws_bridge.js', '/%2e%2e%2fpackage.json', '/%ZZ'])
            assert.equal((await fetch(base + route)).status, 404, route);
    });
    await t.test('origin is required and must match the allowlist', async () => {
        for (const origin of [undefined, 'https://untrusted.invalid']) {
            const ws = new WebSocket(`ws://127.0.0.1:${wsPort}`, origin ? { origin } : {}); browsers.push(ws);
            const [err] = await once(ws, 'error'); assert.match(err.message, /401/);
        }
    });
    await t.test('invalid JSON, shapes, limits and cancel IDs never reach the engine', async () => {
        const before = frames.length;
        const requests = ['{', 'null', '[]', '{}', JSON.stringify({ type: 'unknown' })];
        const good = { type: 'new_order', side: 'BUY', price: 10, quantity: 1 };
        for (const patch of [{ side: 'HOLD' }, { price: '10' }, { price: null }, { price: 4.9999 },
            { price: 15.0001 }, { price: 10.00001 }, { quantity: 0 }, { quantity: -1 },
            { quantity: 1.5 }, { quantity: '1' }, { quantity: 4294967296 }]) requests.push(JSON.stringify({ ...good, ...patch }));
        for (const id of [0, '', '-1', '0', '1.1', '18446744073709551616'])
            requests.push(JSON.stringify({ type: 'cancel', order_id: id }));
        for (const instrument_id of [0, 999, '2', null, -1, 1.5])
            requests.push(JSON.stringify({ ...good, instrument_id }));
        const previous = a.messages.filter(m => m.type === 'error').length;
        for (const request of requests) a.send(request);
        await until(() => a.messages.filter(m => m.type === 'error').length === previous + requests.length, 'Missing validation errors');
        assert.equal(frames.length, before);
    });
    await t.test('binary packing, decimal precision, fragmented ACK and 64-bit IDs', async () => {
        a.send(JSON.stringify({ type: 'new_order', side: 'SELL', price: 10.0001, quantity: 4294967295 }));
        await until(() => a.messages.some(m => m.status === 'ACK'), 'Missing ACK');
        const frame = frames.at(-1); assert.equal(frame.length, 38); assert.equal(frame[16], 2);
        assert.equal(frame.readBigUInt64LE(18), 100001n); assert.equal(frame.readUInt32LE(26), 4294967295);
        const ack = a.messages.find(m => m.status === 'ACK');
        assert.equal(ack.order_id, '9007199254740993'); assert.equal(ack.side, 'SELL');
        assert.equal(ack.remaining, 4294967295); assert.equal(ack.price, 10.0001);
        a.send(JSON.stringify({ type: 'cancel', order_id: ack.order_id }));
        await until(() => frames.at(-1)[2] === 2, 'Cancel was not packed');
        assert.equal(frames.at(-1).length, 16); assert.equal(frames.at(-1).readBigUInt64LE(4), 9007199254740993n);
    });
    await t.test('catalog and nondefault product route through TCP and ACK metadata', async () => {
        assert.deepEqual(a.messages.find(m => m.type === 'catalog').products.map(p => p.id), [1, 2, 3]);
        const ws = await connect();
        ws.send(JSON.stringify({ type: 'new_order', instrument_id: 2, side: 'BUY', price: 10, quantity: 3 }));
        await until(() => ws.messages.some(m => m.status === 'ACK'), 'No product ACK');
        assert.equal(frames.at(-1).readUInt32LE(12), 2);
        assert.equal(ws.messages.find(m => m.status === 'ACK').instrument_id, 2);
        ws.close();
    });
    await t.test('invalid UDP, duplicate sequence filtering and real snapshot readiness', async () => {
        const before = a.messages.length;
        await sendUDP(Buffer.alloc(5));
        const badMagic = snapshot(1); badMagic.writeUInt32LE(0); await sendUDP(badMagic);
        const badVersion = snapshot(2); badVersion.writeUInt16LE(99, 4); await sendUDP(badVersion);
        await sendUDP(snapshot(3, 11)); await delay(50);
        await sendUDP(snapshot(4, 1, 999));
        assert.equal(a.messages.slice(before).filter(m => m.type === 'book_snapshot').length, 0);
        await sendUDP(snapshot(100));
        await until(() => a.messages.some(m => m.type === 'book_snapshot' && m.instrument_id === 1), 'No first-product snapshot');
        assert.equal((await fetch(`${base}/healthz`)).status, 503, 'Other products must also be fresh');
        await sendUDP(snapshot(101, 0, 2)); await sendUDP(snapshot(102, 0, 3));
        await until(() => a.messages.some(m => m.type === 'book_snapshot'), 'No valid snapshot');
        const snap = a.messages.find(m => m.type === 'book_snapshot');
        assert.equal(snap.instrument_id, 1);
        assert.deepEqual(snap.bids, [{ price: 10.0001, qty: 17 }]); assert.equal(snap.stats.p99, 200);
        assert.equal((await fetch(`${base}/healthz`)).status, 200);
        const count = a.messages.filter(m => m.type === 'book_snapshot' && m.instrument_id === 1).length;
        await sendUDP(snapshot(100)); await sendUDP(snapshot(99)); await delay(50);
        assert.equal(a.messages.filter(m => m.type === 'book_snapshot' && m.instrument_id === 1).length, count);
    });
    await t.test('feed timeout becomes unhealthy and sequence reset recovers', async () => {
        await until(() => a.messages.some(m => m.type === 'stale'), 'No stale notification', 6000);
        assert.equal((await fetch(`${base}/healthz`)).status, 503);
        await sendUDP(snapshot(1));
        await sendUDP(snapshot(2, 0, 2)); await sendUDP(snapshot(3, 0, 3));
        await until(() => a.messages.filter(m => m.type === 'book_snapshot' && m.instrument_id === 1).length === 2, 'Reset sequence was not accepted');
        assert.equal((await fetch(`${base}/healthz`)).status, 200);
    });
    await t.test('command burst rate is bounded', async () => {
        const ws = await connect(); for (let i = 0; i < 150; ++i) ws.send('{}');
        await until(() => ws.messages.some(m => m.message?.includes('Rate limit')), 'Rate limit did not activate'); ws.close();
    });
    await t.test('oversized WebSocket payload closes only that session', async () => {
        const ws = await connect(); const closed = once(ws, 'close'); ws.send('x'.repeat(5000));
        const [code] = await closed; assert.equal(code, 1009); assert.equal(a.readyState, WebSocket.OPEN);
    });
    await t.test('malformed engine response closes affected session', async () => {
        const ws = await connect(); corrupt = true;
        const closed = once(ws, 'close');
        ws.send(JSON.stringify({ type: 'new_order', side: 'BUY', price: 10, quantity: 1 }));
        const [code] = await closed; assert.equal(code, 1011); corrupt = false;
    });
    await t.test('upstream loss closes the remaining browser instead of replaying', async () => {
        const closed = once(a, 'close'); for (const socket of sockets) socket.destroy();
        const [code] = await closed; assert.equal(code, 1011);
    });
});
