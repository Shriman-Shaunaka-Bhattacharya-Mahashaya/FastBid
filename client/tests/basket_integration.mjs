import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import net from 'node:net';
import dgram from 'node:dgram';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { WebSocket } from 'ws';

if (!process.argv[2]) throw Error('Pass the engine executable path');
const executable = path.resolve(process.argv[2]);
const cwd = fileURLToPath(new URL('../', import.meta.url));
const children = [], clients = [];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(check, message, timeout = 12000) {
    const deadline = Date.now() + timeout;
    while (!check()) { assert(Date.now() < deadline, message); await delay(10); }
}
async function tcpPort() {
    const server = net.createServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
    const port = server.address().port; await new Promise(resolve => server.close(resolve)); return port;
}
async function udpPort() {
    const socket = dgram.createSocket('udp4'); socket.bind(0, '127.0.0.1'); await once(socket, 'listening');
    const port = socket.address().port; socket.close(); return port;
}
const enginePort = await tcpPort(), bridgePort = await tcpPort(), marketPort = await udpPort();
const env = { ...process.env, FASTBID_TCP_HOST: '127.0.0.1', FASTBID_TCP_PORT: String(enginePort),
    FASTBID_WS_HOST: '127.0.0.1', FASTBID_WS_PORT: String(bridgePort), FASTBID_UDP_MCAST_PORT: String(marketPort),
    FASTBID_ALLOWED_ORIGINS: 'http://localhost:5173' };
function start(command, args) {
    const process = spawn(command, args, { cwd, env, stdio: ['ignore', 'pipe', 'pipe'] });
    process.output = ''; process.stdout.on('data', data => { process.output += data; });
    process.stderr.on('data', data => { process.output += data; }); children.push(process); return process;
}
async function stop(child) {
    if (child.exitCode !== null || child.signalCode !== null) return;
    const exited = once(child, 'exit'); child.kill('SIGTERM');
    const timer = setTimeout(() => child.kill('SIGKILL'), 4000); timer.unref();
    await exited; clearTimeout(timer);
}
async function connect() {
    const ws = new WebSocket(`ws://127.0.0.1:${bridgePort}`, { origin: 'http://localhost:5173' });
    clients.push(ws); ws.messages = []; ws.on('error', () => {});
    ws.on('message', data => ws.messages.push(JSON.parse(data)));
    await until(() => ws.messages.some(m => m.type === 'ready'), 'Browser engine connection never became ready');
    await until(() => ws.messages.some(m => m.type === 'catalog' && m.groups_ready), 'Registry did not synchronize'); return ws;
}
function send(ws, request) { ws.send(JSON.stringify(request)); }
const recipe = { type: 'create_group', name: 'Integration Two and Three',
    legs: [{ instrument_id: 1, quantity: 2 }, { instrument_id: 2, quantity: 3 }] };

try {
    let engine = start(executable, []);
    await until(() => engine.output.includes('[Main] Ready'), 'Engine failed to start: ' + engine.output, 20000);
    let bridge = start(process.execPath, ['ws_bridge.js']);
    await until(() => bridge.output.includes('[Bridge] Listening'), 'Bridge failed to start: ' + bridge.output);
    const a = await connect(), b = await connect();
    // The two browser commands arrive without waiting for either registration result.
    send(a, recipe); send(b, { ...recipe, name: 'Equivalent recipe', legs: recipe.legs.slice().reverse() });
    await until(() => a.messages.some(m => m.type === 'group_created') && b.messages.some(m => m.type === 'group_created'), 'Creation acknowledgments missing');
    const groupA = a.messages.find(m => m.type === 'group_created').product;
    const groupB = b.messages.find(m => m.type === 'group_created').product;
    assert.equal(groupA.id, groupB.id); assert.deepEqual(groupA.legs, recipe.legs); assert.deepEqual(groupB.legs, recipe.legs);
    assert(groupA.id >= 0x80000000); assert.equal(groupA.tick_size_ticks, 5);
    const latest = b.messages.filter(m => m.type === 'catalog').at(-1);
    assert.equal(latest.products.filter(p => p.kind === 'basket').length, 1);

    send(a, { type: 'new_order', instrument_id: groupA.id, side: 'BUY', price: 50, quantity: 3 });
    await until(() => a.messages.some(m => m.status === 'ACK'), 'Basket buy ACK missing');
    const buy = a.messages.find(m => m.status === 'ACK'); assert.equal(buy.instrument_id, groupA.id);
    send(b, { type: 'new_order', instrument_id: groupA.id, side: 'SELL', price: 50, quantity: 1 });
    await until(() => a.messages.some(m => m.status === 'FILL') && b.messages.some(m => m.status === 'FILL'), 'Basket match missing');
    const fill = a.messages.find(m => m.status === 'FILL');
    assert.equal(fill.remaining, 2); assert.equal(fill.basket_units, 1);
    assert.deepEqual(fill.constituent_fills, [{ instrument_id: 1, quantity: 2 }, { instrument_id: 2, quantity: 3 }]);
    send(a, { type: 'cancel', instrument_id: groupA.id, order_id: buy.order_id });
    await until(() => a.messages.some(m => m.status === 'CANCEL_ACK'), 'Basket remainder cancel failed');
    await until(() => a.messages.some(m => m.type === 'trade' && m.instrument_id === groupA.id), 'Basket public trade missing');

    // Stop only the bridge. The matching engine and its immutable registry remain alive.
    const closed = once(a, 'close'); await stop(bridge); await closed;
    assert.equal(engine.exitCode, null);
    bridge = start(process.execPath, ['ws_bridge.js']);
    await until(() => bridge.output.includes('[Bridge] Listening'), 'Restarted bridge did not listen');
    const c = await connect();
    const restored = c.messages.filter(m => m.type === 'catalog' && m.groups_ready).at(-1).products.find(p => p.id === groupA.id);
    assert(restored, 'Engine-owned group missing after bridge restart'); assert.deepEqual(restored.legs, recipe.legs);
    send(c, { type: 'new_order', instrument_id: restored.id, side: 'SELL', price: 50, quantity: 2 });
    await until(() => c.messages.some(m => m.status === 'ACK'), 'Restored group cannot trade');
    const restoredOrder = c.messages.find(m => m.status === 'ACK');
    send(c, { type: 'cancel', instrument_id: restored.id, order_id: restoredOrder.order_id });
    await until(() => c.messages.some(m => m.status === 'CANCEL_ACK'), 'Restored group cannot cancel');
    const clientClosed = once(c, 'close'); await stop(engine); await clientClosed;
    engine = start(executable, []);
    await until(() => engine.output.includes('[Main] Ready'), 'Restarted engine failed to start', 20000);
    const d = await connect();
    const resetCatalog = d.messages.filter(m => m.type === 'catalog' && m.groups_ready).at(-1);
    assert.equal(resetCatalog.products.filter(p => p.kind === 'basket').length, 0, 'Engine restart must not invent old definitions');
    send(d, { type: 'new_order', instrument_id: groupA.id, side: 'BUY', price: 50, quantity: 1 });
    await until(() => d.messages.some(m => m.type === 'error' && m.message === 'Unknown product'), 'Stale basket ID remained tradable');
    console.log('Basket integration passed: concurrent canonical creation, whole-unit fills, exact component ratios, cancellation, public trade, authoritative bridge resync and engine-restart invalidation.');
} finally {
    for (const ws of clients) ws.terminate();
    for (const child of children.reverse()) await stop(child);
    for (const child of children) assert(!child.output.includes('AddressSanitizer') && !child.output.includes('runtime error:'), child.output);
}
