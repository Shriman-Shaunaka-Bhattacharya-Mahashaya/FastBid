import { test } from 'node:test';
import assert from 'node:assert/strict';
import net from 'node:net';
import { once } from 'node:events';
import { products } from '../catalog.mjs';
import { BasketControl, BASKET_ID_BASE, validateGroup, encodeGroup, decodeGroup } from '../baskets.mjs';

const group = { name: 'Fruit Mix', legs: [{ instrument_id: 1, quantity: 2 }, { instrument_id: 2, quantity: 3 }] };
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate, message = 'Condition timed out') {
    const deadline = Date.now() + 5000;
    while (!predicate()) { assert(Date.now() < deadline, message); await pause(5); }
}
function report(requestId, definition, type = 14, reason = 0) {
    const b = Buffer.alloc(236); b.writeUInt16LE(236); b[2] = type;
    b.writeBigUInt64LE(requestId, 4); b.writeBigUInt64LE(BigInt(reason), 12);
    if (!definition) return b;
    const total = definition.legs.reduce((n, leg) => n + leg.quantity, 0);
    b.writeUInt32LE(definition.id, 20); b.writeUInt32LE(total, 24);
    b.writeBigUInt64LE(BigInt(total * 50000), 28); b.writeBigUInt64LE(BigInt(total * 150000), 36);
    b.writeBigUInt64LE(BigInt(total * 100000), 44); b.write(definition.name, 52, 48, 'ascii');
    b.writeUInt16LE(definition.legs.length, 100);
    definition.legs.forEach((leg, i) => { b.writeUInt32LE(leg.instrument_id, 104 + i * 8); b.writeUInt32LE(leg.quantity, 108 + i * 8); });
    return b;
}

test('basket recipes canonicalize leg order without reducing share ratios', () => {
    assert.deepEqual(validateGroup({ ...group, name: '  Fruit Mix  ', legs: group.legs.slice().reverse() }, products), group);
    const doubled = validateGroup({ name: 'Doubled', legs: group.legs.map(leg => ({ ...leg, quantity: leg.quantity * 2 })) }, products);
    assert.notDeepEqual(doubled.legs, group.legs);
    assert.equal(doubled.legs[0].quantity, 4);
});

test('basket validation rejects invalid names, legs, quantities and nested groups', () => {
    for (const name of ['', '  ', 'x'.repeat(49), 'Fruit\nMix', 'Früt', null])
        assert.throws(() => validateGroup({ ...group, name }, products));
    for (const legs of [[], [group.legs[0]], [group.legs[0], group.legs[0]],
        [{ instrument_id: BASKET_ID_BASE, quantity: 1 }, group.legs[0]],
        [{ instrument_id: '1', quantity: 1 }, group.legs[1]],
        ...[0, -1, 1.5, 1001, '2', null].map(quantity => [{ instrument_id: 1, quantity }, group.legs[1]])])
        assert.throws(() => validateGroup({ ...group, legs }, products));
    const many = Array.from({ length: 16 }, (_, i) => ({ id: i + 1 }));
    assert.throws(() => validateGroup({ name: 'Oversized', legs: many.map(p => ({ instrument_id: p.id, quantity: 1000 })) }, many));
});

test('control encoding is fixed-size and preserves exact quantities', () => {
    const b = encodeGroup(9007199254740993n, group);
    assert.equal(b.length, 192); assert.equal(b.readUInt16LE(0), 192); assert.equal(b[2], 4);
    assert.equal(b.readBigUInt64LE(4), 9007199254740993n); assert.equal(b.readUInt16LE(12), 2);
    assert.equal(b.subarray(16, 25).toString(), 'Fruit Mix');
    assert.equal(b.readUInt32LE(64), 1); assert.equal(b.readUInt32LE(68), 2);
    assert.equal(b.readUInt32LE(72), 2); assert.equal(b.readUInt32LE(76), 3);
    assert(b.subarray(80).every(byte => byte === 0));
});

test('engine definition decoder validates capacity, precision and padding', () => {
    const valid = report(1n, { ...group, id: BASKET_ID_BASE });
    const decoded = decodeGroup(valid, products);
    assert.equal(decoded.kind, 'basket'); assert.equal(decoded.min_price, 25); assert.equal(decoded.max_price, 75);
    assert.equal(decoded.tick_size_ticks, 5); assert.deepEqual(decoded.legs, group.legs);
    for (const corrupt of [b => b.writeUInt32LE(1, 232), b => b.writeUInt32LE(0, 24),
        b => b.writeUInt32LE(1, 20), b => b.writeUInt32LE(BASKET_ID_BASE + 8, 20),
        b => b.writeUInt16LE(17, 100), b => { b[52] = 255; },
        b => b.writeBigUInt64LE(2n ** 63n, 36), b => { b[120] = 1; }]) {
        const b = Buffer.from(valid); corrupt(b); assert.throws(() => decodeGroup(b, products));
    }
    assert.throws(() => decodeGroup(valid.subarray(0, 232), products));
});

test('unresponsive control synchronization has a bounded deadline', { timeout: 5000 }, async t => {
    const sockets = new Set();
    const server = net.createServer(socket => {
        sockets.add(socket); socket.on('close', () => sockets.delete(socket)); socket.on('error', () => {});
        socket.on('data', () => {}); // Deliberately never acknowledge SYNC.
    });
    server.listen(0, '127.0.0.1'); await once(server, 'listening');
    let losses = 0;
    const control = new BasketControl({ host: '127.0.0.1', port: server.address().port, baseProducts: products,
        syncTimeoutMs: 25, reconnectMs: 1000, onLoss: () => { ++losses; } });
    t.after(async () => { control.close(); for (const socket of sockets) socket.destroy(); await new Promise(resolve => server.close(resolve)); });
    control.start(); await until(() => losses === 1);
    assert.equal(control.ready, false); assert.equal(control.pending.size, 0);
});

test('control registry acknowledges, deduplicates, bounds and resynchronizes authoritatively', { timeout: 20000 }, async t => {
    const connections = new Set(), registry = new Map(), created = [];
    let current, autoRegister = false, autoSync = true, failedReason = 0, nextId = BASKET_ID_BASE;
    const server = net.createServer(socket => {
        connections.add(socket); current = { socket, sync: null };
        socket.on('close', () => connections.delete(socket)); socket.on('error', () => {});
        let rx = Buffer.alloc(0);
        socket.on('data', bytes => {
            rx = Buffer.concat([rx, bytes]);
            while (rx.length >= 4 && rx.length >= rx.readUInt16LE(0)) {
                const length = rx.readUInt16LE(0), frame = Buffer.from(rx.subarray(0, length)); rx = rx.subarray(length);
                if (frame[2] === 5) {
                    current.sync = frame.readBigUInt64LE(4);
                    if (autoSync) sync(current);
                } else {
                    assert.equal(frame[2], 4); assert.equal(frame.length, 192);
                    const count = frame.readUInt16LE(12), nameBytes = frame.subarray(16, 64);
                    const request = { id: frame.readBigUInt64LE(4), name: nameBytes.toString('ascii').replace(/\0+$/, ''),
                        legs: Array.from({ length: count }, (_, i) => ({ instrument_id: frame.readUInt32LE(64 + i * 8), quantity: frame.readUInt32LE(68 + i * 8) })), socket };
                    created.push(request); if (autoRegister) acknowledge(request);
                }
            }
        });
    });
    function sync(connection) {
        for (const product of registry.values()) connection.socket.write(report(connection.sync, product));
        connection.socket.write(report(connection.sync, null, 15));
    }
    function acknowledge(request) {
        if (failedReason) { request.socket.write(report(request.id, null, 14, failedReason)); failedReason = 0; return; }
        const key = JSON.stringify(request.legs);
        if (!registry.has(key)) registry.set(key, { id: nextId++, name: request.name, legs: request.legs });
        const b = report(request.id, registry.get(key));
        request.socket.write(b.subarray(0, 3)); setTimeout(() => { if (!request.socket.destroyed) request.socket.write(b.subarray(3)); }, 5);
    }
    server.listen(0, '127.0.0.1'); await once(server, 'listening');
    let losses = 0;
    const control = new BasketControl({ host: '127.0.0.1', port: server.address().port, baseProducts: products,
        reconnectMs: 20, onLoss: () => { ++losses; } });
    t.after(async () => {
        control.close(); for (const socket of connections) socket.destroy();
        await new Promise(resolve => server.close(resolve));
    });
    control.start(); await until(() => control.ready);

    await t.test('basket is hidden until the exact engine acknowledgment', async () => {
        const pending = control.create(group); await until(() => created.length === 1);
        assert.equal(control.groups.size, 0); acknowledge(created[0]);
        const product = await pending; assert.equal(product.id, BASKET_ID_BASE); assert.equal(control.groups.size, 1);
    });
    await t.test('reordered identical recipe reuses ID and preserves its first label', async () => {
        autoRegister = true;
        const product = await control.create({ name: 'Another label', legs: group.legs.slice().reverse() });
        assert.equal(product.id, BASKET_ID_BASE); assert.equal(product.name, group.name); assert.equal(control.groups.size, 1);
        const different = await control.create({ name: 'More shares', legs: group.legs.map(leg => ({ ...leg, quantity: leg.quantity * 2 })) });
        assert.notEqual(different.id, product.id); assert.equal(control.groups.size, 2);
    });
    await t.test('engine registration rejection is reported without inventing catalog entries', async () => {
        failedReason = 10;
        await assert.rejects(control.create(group), /registry is full/); assert.equal(control.groups.size, 2);
    });
    await t.test('loss fails closed, clears metadata and restores engine-owned IDs on reconnect', async () => {
        autoSync = false; current.socket.destroy();
        await until(() => losses === 1); assert.equal(control.ready, false); assert.equal(control.groups.size, 0);
        await assert.rejects(control.create(group), /synchronizing/);
        await until(() => current.sync !== null && !current.socket.destroyed);
        sync(current); await until(() => control.ready);
        assert.equal(control.groups.size, 2); assert.equal(control.groups.get(BASKET_ID_BASE).name, group.name);
    });
    await t.test('pending creation count is bounded and ambiguous requests fail on protocol loss', async () => {
        autoRegister = false;
        const pending = Array.from({ length: 64 }, () => control.create(group).catch(error => error));
        await assert.rejects(control.create(group), /Too many pending/);
        await until(() => control.pending.size === 64);
        const b = Buffer.from([0, 0, 99, 0]); current.socket.write(b);
        const results = await Promise.all(pending);
        assert(results.every(value => value instanceof Error));
        assert(results.every(value => value.message.includes('may have completed')));
    });
});
