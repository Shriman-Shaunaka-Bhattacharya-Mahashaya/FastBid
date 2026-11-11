import { test } from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import dgram from 'node:dgram';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';
import { WebSocket } from 'ws';

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check, message) {
    const deadline = Date.now() + 8000;
    while (!check()) { assert(Date.now() < deadline, message); await delay(10); }
}
async function port() {
    const server = net.createServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
    const n = server.address().port; await new Promise(resolve => server.close(resolve)); return n;
}
function groupResponse(requestId, definition, type = 14, reason = 0) {
    const b = Buffer.alloc(236); b.writeUInt16LE(236); b[2] = type;
    b.writeBigUInt64LE(requestId, 4); b.writeBigUInt64LE(BigInt(reason), 12);
    if (!definition) return b;
    b.writeUInt32LE(definition.id, 20); b.writeUInt32LE(5, 24);
    b.writeBigUInt64LE(250000n, 28); b.writeBigUInt64LE(750000n, 36); b.writeBigUInt64LE(500000n, 44);
    b.write(definition.name, 52, 48); b.writeUInt16LE(2, 100);
    b.writeUInt32LE(1, 104); b.writeUInt32LE(2, 108); b.writeUInt32LE(2, 112); b.writeUInt32LE(3, 116);
    return b;
}

test('browser group creation is engine-acknowledged, price-aligned and visible to all sessions', { timeout: 20000 }, async t => {
    const tcpPort = await port(), wsPort = await port();
    const reserveUDP = dgram.createSocket('udp4'); reserveUDP.bind(0, '127.0.0.1'); await once(reserveUDP, 'listening');
    const udpPort = reserveUDP.address().port; reserveUDP.close();
    const sockets = new Set(), browsers = [], frames = [];
    let control, syncId, register, definition;
    const fake = net.createServer(socket => {
        sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
        let rx = Buffer.alloc(0);
        socket.on('data', data => {
            rx = Buffer.concat([rx, data]);
            while (rx.length >= 4 && rx.length >= rx.readUInt16LE(0)) {
                const len = rx.readUInt16LE(0), b = Buffer.from(rx.subarray(0, len)); rx = rx.subarray(len);
                frames.push(b);
                if (b[2] === 5) {
                    control = socket; syncId = b.readBigUInt64LE(4);
                    if (definition) {
                        socket.write(groupResponse(syncId, definition)); socket.write(groupResponse(syncId, null, 15));
                    }
                } else if (b[2] === 4) register = b;
                else if (b[2] === 1) {
                    const ack = Buffer.alloc(20); ack.writeUInt16LE(20); ack[2] = 10;
                    ack.writeBigUInt64LE(100n, 4); ack.writeBigUInt64LE(b.readBigUInt64LE(4), 12); socket.write(ack);
                    const fill = Buffer.alloc(36); fill.writeUInt16LE(36); fill[2] = 11;
                    fill.writeBigUInt64LE(100n, 4); fill.writeBigUInt64LE(8n, 12);
                    fill.writeBigUInt64LE(b.readBigUInt64LE(18), 20); fill.writeUInt32LE(b.readUInt32LE(26), 28); socket.write(fill);
                }
            }
        });
    });
    fake.listen(tcpPort, '127.0.0.1'); await once(fake, 'listening');
    const child = spawn(process.execPath, ['ws_bridge.js'], { cwd: fileURLToPath(new URL('../', import.meta.url)),
        env: { ...process.env, FASTBID_TCP_PORT: String(tcpPort), FASTBID_TCP_HOST: '127.0.0.1',
            FASTBID_WS_PORT: String(wsPort), FASTBID_WS_HOST: '127.0.0.1', FASTBID_UDP_MCAST_PORT: String(udpPort),
            FASTBID_UDP_MCAST_ADDR: '239.255.0.1', FASTBID_ALLOWED_ORIGINS: 'http://localhost:5173' },
        stdio: ['ignore', 'pipe', 'pipe'] });
    let output = ''; child.stdout.on('data', data => { output += data; }); child.stderr.on('data', data => { output += data; });
    t.after(async () => {
        for (const ws of browsers) ws.terminate();
        if (child.exitCode === null) { const closed = once(child, 'exit'); child.kill('SIGTERM'); const timer = setTimeout(() => child.kill('SIGKILL'), 3000); await closed; clearTimeout(timer); }
        for (const socket of sockets) socket.destroy(); await new Promise(resolve => fake.close(resolve));
    });
    await until(() => output.includes('[Bridge] Listening'), output);
    async function connect() {
        const ws = new WebSocket(`ws://127.0.0.1:${wsPort}`, { origin: 'http://localhost:5173' });
        browsers.push(ws); ws.messages = []; ws.on('error', () => {}); ws.on('message', data => ws.messages.push(JSON.parse(data)));
        await until(() => ws.messages.some(m => m.type === 'ready'), 'No ready message'); return ws;
    }
    const a = await connect(), b = await connect();
    const request = { type: 'create_group', name: 'Two and Three', legs: [{ instrument_id: 2, quantity: 3 }, { instrument_id: 1, quantity: 2 }] };

    await t.test('unsynchronized control cannot create a bridge-only group', async () => {
        a.send(JSON.stringify(request));
        await until(() => a.messages.some(m => m.type === 'error' && /synchronizing/.test(m.message)), 'Missing sync error');
        assert(!register); assert(!a.messages.some(m => m.type === 'group_created'));
        await until(() => control && syncId, 'Missing sync request'); control.write(groupResponse(syncId, null, 15));
        await until(() => a.messages.some(m => m.type === 'catalog' && m.groups_ready), 'Catalog did not become ready');
    });
    await t.test('only an engine ACK exposes the canonical group to both browsers', async () => {
        a.send(JSON.stringify(request)); await until(() => register, 'Missing registration request');
        assert.equal(register.length, 192); assert.equal(register.readUInt32LE(64), 1); assert.equal(register.readUInt32LE(68), 2);
        assert(!a.messages.some(m => m.type === 'group_created'));
        assert(!b.messages.some(m => m.type === 'catalog' && m.products.length > 3));
        definition = { id: 0x80000000, name: request.name };
        control.write(groupResponse(register.readBigUInt64LE(4), definition));
        await until(() => a.messages.some(m => m.type === 'group_created'), 'Missing creation ACK');
        await until(() => b.messages.some(m => m.type === 'catalog' && m.products.some(p => p.id === definition.id)), 'Second browser missed basket');
        assert.equal(a.messages.find(m => m.type === 'group_created').product.tick_size_ticks, 5);
    });
    await t.test('basket price grid is enforced and whole-unit fills expand constituent ratios', async () => {
        const before = frames.filter(frame => frame[2] === 1).length;
        a.send(JSON.stringify({ type: 'new_order', instrument_id: definition.id, side: 'BUY', price: 50.0001, quantity: 3 }));
        await until(() => a.messages.some(m => m.type === 'error' && /increments/.test(m.message)), 'Missing price-grid error');
        assert.equal(frames.filter(frame => frame[2] === 1).length, before);
        a.send(JSON.stringify({ type: 'new_order', instrument_id: definition.id, side: 'BUY', price: 50.0005, quantity: 3 }));
        await until(() => a.messages.some(m => m.status === 'FILL'), 'Missing basket fill');
        const fill = a.messages.find(m => m.status === 'FILL'); assert.equal(fill.instrument_id, definition.id);
        assert.equal(fill.basket_units, 3);
        assert.deepEqual(fill.constituent_fills, [{ instrument_id: 1, quantity: 6 }, { instrument_id: 2, quantity: 9 }]);
        assert(!b.messages.some(m => m.status === 'FILL'));
    });
    await t.test('control loss closes sessions and next connection receives the restored engine catalog', async () => {
        const closedA = once(a, 'close'), closedB = once(b, 'close'); control.destroy();
        assert.equal((await closedA)[0], 1011); assert.equal((await closedB)[0], 1011);
        await until(() => control && !control.destroyed, 'Control did not reconnect');
        const c = await connect();
        await until(() => c.messages.some(m => m.type === 'catalog' && m.groups_ready && m.products.some(p => p.id === definition.id)), 'Engine basket was not restored');
    });
});
