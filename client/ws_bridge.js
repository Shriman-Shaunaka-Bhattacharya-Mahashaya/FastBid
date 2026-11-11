import dgram from 'node:dgram';
import net from 'node:net';
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';
import { products } from './catalog.mjs';
const productById = new Map(products.map(p => [p.id, p]));

const host = process.env.FASTBID_WS_HOST || '127.0.0.1';
const port = Number(process.env.FASTBID_WS_PORT || 8080);
const origins = new Set((process.env.FASTBID_ALLOWED_ORIGINS || 'http://localhost:5173,http://127.0.0.1:5173').split(','));
const staticRoot = path.resolve(fileURLToPath(new URL('./dist/', import.meta.url)));
const server = http.createServer(async (req, res) => {
    if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); res.end(); return; }
    const pathname = new URL(req.url, 'http://localhost').pathname;
    if (pathname === '/healthz') {
        const ready = products.every(p => Date.now() - (snapshots.get(p.id)?.at || 0) < 2000);
        res.writeHead(ready ? 200 : 503, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
        res.end(JSON.stringify({ ready, mode: 'paper' })); return;
    }
    try {
        const relative = decodeURIComponent(pathname === '/' ? '/index.html' : pathname);
        const file = path.resolve(staticRoot, '.' + relative);
        if (!file.startsWith(staticRoot + path.sep) && file !== path.join(staticRoot, 'index.html')) throw Error('Invalid path');
        const data = await readFile(file);
        const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.ico': 'image/x-icon' }[path.extname(file)] || 'application/octet-stream';
        res.writeHead(200, { 'Content-Type': mime, 'X-Content-Type-Options': 'nosniff', 'Cache-Control': 'no-cache' });
        res.end(req.method === 'HEAD' ? undefined : data);
    } catch { res.writeHead(404); res.end('Not found'); }
});
const wss = new WebSocketServer({ server, maxPayload: 4096, perMessageDeflate: false,
    verifyClient: ({ origin }) => origins.has(origin) });
const clients = new Set();
const snapshots = new Map();
let lastSnapshotAt = 0;
let lastSequence = 0n;
let orderId = 0n;
const send = (ws, msg) => {
    if (ws.readyState !== 1) return;
    if (ws.bufferedAmount > 65536) { ws.close(1013, 'Slow consumer'); return; }
    ws.send(JSON.stringify(msg));
};
const broadcast = msg => { for (const ws of clients) send(ws, msg); };
const error = (ws, message) => send(ws, { type: 'error', message });

// One TCP session per WS client preserves cancellation ownership and report routing.
// A lost session is closed, never silently retried: acceptance may be ambiguous.
wss.on('connection', ws => {
    if (clients.size >= 256) { ws.close(1013, 'Connection limit'); return; }
    clients.add(ws);
    let ready = false, blocked = false, rx = Buffer.alloc(0);
    const pending = new Map();
    const openOrders = new Map();
    const pendingCancels = new Map();
    let cancelCount = 0;
    let tokens = 100, lastRefill = Date.now();
    const tcp = net.createConnection({ host: process.env.FASTBID_TCP_HOST || '127.0.0.1', port: Number(process.env.FASTBID_TCP_PORT || 9000) });
    send(ws, { type: 'catalog', products });
    tcp.setNoDelay(true);
    tcp.on('connect', () => { ready = true; send(ws, { type: 'ready' }); });
    tcp.on('error', () => { error(ws, 'Engine unavailable; reconnect to open a new session'); });
    tcp.on('close', () => { ready = false; ws.close(1011, 'Engine session closed'); });
    tcp.on('drain', () => { blocked = false; });
    tcp.on('data', data => {
        rx = Buffer.concat([rx, data]);
        if (rx.length > 131072) { tcp.destroy(); return; }
        while (rx.length >= 4) {
            const len = rx.readUInt16LE(0), type = rx[2];
            const expected = { 10: 20, 11: 36, 12: 20, 13: 28 }[type];
            if (!expected || len !== expected || rx[3]) { tcp.destroy(); return; }
            if (rx.length < len) break;
            const msg = { type: 'execution_report', status: { 10: 'ACK', 11: 'FILL', 12: 'CANCEL_ACK', 13: 'REJECT' }[type], order_id: rx.readBigUInt64LE(4).toString() };
            if (type === 11) Object.assign(msg, { trade_id: rx.readBigUInt64LE(12).toString(), price: Number(rx.readBigUInt64LE(20)) / 10000, quantity: rx.readUInt32LE(28), remaining: rx.readUInt32LE(32) });
            else msg.client_order_id = rx.readBigUInt64LE(12).toString();
            if (type === 13) msg.reason = Number(rx.readBigUInt64LE(20));
            if (type === 10) {
                Object.assign(msg, pending.get(msg.client_order_id));
                openOrders.set(msg.order_id, { instrument_id: msg.instrument_id, side: msg.side });
            } else if (type === 11) Object.assign(msg, openOrders.get(msg.order_id));
            else if (type === 12 || (type === 13 && msg.client_order_id === '0')) {
                const queue = pendingCancels.get(msg.order_id);
                msg.instrument_id = queue?.shift() ?? openOrders.get(msg.order_id)?.instrument_id;
                if (queue) { --cancelCount; if (!queue.length) pendingCancels.delete(msg.order_id); }
            } else if (type === 13) msg.instrument_id = pending.get(msg.client_order_id)?.instrument_id;
            if (type === 12 || (type === 11 && msg.remaining === 0)) openOrders.delete(msg.order_id);
            if (type === 10 || type === 13) pending.delete(msg.client_order_id);
            send(ws, msg); rx = rx.subarray(len);
        }
    });
    for (const { message, at } of snapshots.values()) if (Date.now() - at < 2000) send(ws, message);
    ws.on('message', data => {
        try {
            const now = Date.now(); tokens = Math.min(100, tokens + (now - lastRefill) / 10); lastRefill = now;
            if (tokens < 1) { error(ws, 'Rate limit: 100 requests/s'); return; } --tokens;
            if (!ready || blocked) { error(ws, 'Engine not ready or backpressured; request not sent'); return; }
            const req = JSON.parse(data);
            const instrument_id = req.instrument_id === undefined ? products[0].id : req.instrument_id;
            const product = productById.get(instrument_id);
            if (!Number.isInteger(instrument_id) || !product) throw Error('Unknown product');
            let buf;
            if (req.type === 'new_order') {
                if (pending.size >= 1024) throw Error('Too many pending requests');
                if (!['BUY', 'SELL'].includes(req.side) || typeof req.price !== 'number' || !Number.isFinite(req.price) ||
                    !Number.isInteger(req.quantity) || req.quantity <= 0 || req.quantity > 4294967295) throw Error('Invalid side, price or quantity');
                const ticks = Math.round(req.price * 10000);
                if (ticks < product.min_price_ticks || ticks > product.max_price_ticks || Math.abs(req.price * 10000 - ticks) > 0.000001) throw Error(`Price must be $${product.min_price}-$${product.max_price} with at most four decimal places`);
                buf = Buffer.alloc(38); buf.writeUInt16LE(38); buf[2] = 1;
                buf.writeBigUInt64LE(++orderId, 4); buf.writeUInt32LE(instrument_id, 12); buf[16] = req.side === 'BUY' ? 1 : 2; buf[17] = 1;
                buf.writeBigUInt64LE(BigInt(ticks), 18); buf.writeUInt32LE(req.quantity, 26);
                buf.writeBigUInt64LE(BigInt(Date.now()) * 1000000n, 30);
            } else if (req.type === 'cancel') {
                if (cancelCount >= 1024) throw Error('Too many pending requests');
                if (typeof req.order_id !== 'string' || !/^[1-9][0-9]{0,19}$/.test(req.order_id) || BigInt(req.order_id) > 18446744073709551615n) throw Error('Invalid order ID');
                buf = Buffer.alloc(16); buf.writeUInt16LE(16); buf[2] = 2;
                buf.writeBigUInt64LE(BigInt(req.order_id), 4); buf.writeUInt32LE(instrument_id, 12);
            } else throw Error('Unsupported command');
            if (req.type === 'new_order') pending.set(orderId.toString(), { instrument_id, side: req.side, price: req.price, remaining: req.quantity });
            if (req.type === 'cancel') {
                const queue = pendingCancels.get(req.order_id) || []; queue.push(instrument_id);
                pendingCancels.set(req.order_id, queue); ++cancelCount;
            }
            blocked = !tcp.write(buf);
        } catch (err) { error(ws, err.message || 'Invalid request'); }
    });
    ws.on('error', () => tcp.destroy());
    ws.on('close', () => { clients.delete(ws); tcp.destroy(); });
});
const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
udp.on('error', err => console.error('[UDP]', err.message));
udp.on('listening', () => { udp.addMembership(process.env.FASTBID_UDP_MCAST_ADDR || '239.255.0.1'); });
udp.on('message', msg => {
    if (msg.length < 24 || msg.readUInt32LE(0) !== 0x31444246 || msg.readUInt16LE(4) !== 2 || msg.readUInt32LE(20)) return;
    const kind = msg.readUInt16LE(6), seq = msg.readBigUInt64LE(8), instrument_id = msg.readUInt32LE(16);
    if (!productById.has(instrument_id)) return;
    if (seq <= lastSequence && Date.now() - lastSnapshotAt < 2000) return;
    if (kind === 2 && msg.length === 392) {
        const bids = msg.readUInt32LE(24), asks = msg.readUInt32LE(28);
        if (bids > 10 || asks > 10) return;
        const levels = (offset, count) => Array.from({ length: count }, (_, i) => ({ price: Number(msg.readBigUInt64LE(offset + i * 16)) / 10000, qty: Number(msg.readBigUInt64LE(offset + i * 16 + 8)) }));
        const message = { type: 'book_snapshot', instrument_id, bids: levels(32, bids), asks: levels(192, asks), stats: {
            p50: Number(msg.readBigUInt64LE(352)), p99: Number(msg.readBigUInt64LE(360)), ops: Number(msg.readBigUInt64LE(368)), samples: Number(msg.readBigUInt64LE(376)), dropped: Number(msg.readBigUInt64LE(384)) } };
        lastSnapshotAt = Date.now(); lastSequence = seq;
        snapshots.set(instrument_id, { message, at: lastSnapshotAt }); broadcast(message);
    } else if (kind === 1 && msg.length === 48) {
        if (![1, 2].includes(msg[44])) return;
        lastSequence = seq;
        broadcast({ type: 'trade', instrument_id, trade_id: msg.readBigUInt64LE(24).toString(), price: Number(msg.readBigUInt64LE(32)) / 10000, quantity: msg.readUInt32LE(40), aggressor_side: msg[44] === 1 ? 'BUY' : 'SELL' });
    }
});
udp.bind(Number(process.env.FASTBID_UDP_MCAST_PORT || 5000));
const heartbeat = setInterval(() => {
    for (const p of products) if (Date.now() - (snapshots.get(p.id)?.at || 0) > 2000) broadcast({ type: 'stale', instrument_id: p.id });
    for (const ws of clients) { if (ws.alive === false) { ws.terminate(); continue; } ws.alive = false; ws.ping(); }
}, 2000);
wss.on('connection', ws => { ws.alive = true; ws.on('pong', () => { ws.alive = true; }); });
function shutdown() { clearInterval(heartbeat); for (const ws of clients) ws.terminate(); wss.close(); server.close(); udp.close(); }
process.on('SIGTERM', shutdown); process.on('SIGINT', shutdown);
server.listen(port, host, () => console.log(`[Bridge] Listening on ${host}:${port}`));
