import net from 'node:net';

export const BASKET_ID_BASE = 0x80000000;
export const MAX_BASKETS = 8;
export const MAX_BASKET_LEGS = 16;
export const MAX_LEG_QUANTITY = 1000;
export const MAX_BASKET_SHARES = 10000;

// Composition, rather than the label or UI ordering, identifies a basket.
export function validateGroup(request, baseProducts) {
    if (typeof request?.name !== 'string') throw Error('Enter a basket name');
    const name = request.name.trim();
    if (!name || name.length > 48 || !/^[\x20-\x7e]+$/.test(name))
        throw Error('Basket name must contain 1-48 printable ASCII characters');
    if (!Array.isArray(request.legs) || request.legs.length < 2 || request.legs.length > MAX_BASKET_LEGS)
        throw Error('A basket needs 2-16 different base products');
    const ids = new Set(baseProducts.map(p => p.id)), used = new Set();
    let total = 0;
    const legs = request.legs.map(leg => {
        if (!Number.isInteger(leg?.instrument_id) || !ids.has(leg.instrument_id) || used.has(leg.instrument_id))
            throw Error('Basket legs must use different known base products; nested baskets are not allowed');
        if (!Number.isInteger(leg.quantity) || leg.quantity < 1 || leg.quantity > MAX_LEG_QUANTITY)
            throw Error('Each product quantity must be a whole number from 1 to 1000');
        used.add(leg.instrument_id); total += leg.quantity;
        return { instrument_id: leg.instrument_id, quantity: leg.quantity };
    });
    if (total > MAX_BASKET_SHARES) throw Error('A basket may contain at most 10000 shares');
    legs.sort((a, b) => a.instrument_id - b.instrument_id);
    return { name, legs };
}

export function encodeGroup(requestId, group) {
    const frame = Buffer.alloc(192);
    frame.writeUInt16LE(192); frame[2] = 4;
    frame.writeBigUInt64LE(requestId, 4); frame.writeUInt16LE(group.legs.length, 12);
    frame.write(group.name, 16, 48, 'ascii');
    group.legs.forEach((leg, i) => {
        frame.writeUInt32LE(leg.instrument_id, 64 + i * 8);
        frame.writeUInt32LE(leg.quantity, 68 + i * 8);
    });
    return frame;
}

function safeNumber(frame, offset) {
    const value = frame.readBigUInt64LE(offset);
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw Error('Basket price exceeds supported precision');
    return Number(value);
}

export function decodeGroup(frame, baseProducts) {
    if (frame.length !== 236 || frame.readUInt32LE(232)) throw Error('Malformed basket response size or padding');
    const id = frame.readUInt32LE(20), step = frame.readUInt32LE(24);
    const minimum = safeNumber(frame, 28), maximum = safeNumber(frame, 36), reference = safeNumber(frame, 44);
    const nameBytes = frame.subarray(52, 100), zero = nameBytes.indexOf(0);
    if (zero !== -1 && nameBytes.subarray(zero).some(value => value !== 0)) throw Error('Malformed basket name padding');
    if (nameBytes.subarray(0, zero === -1 ? 48 : zero).some(value => value < 32 || value > 126)) throw Error('Malformed basket name');
    const name = nameBytes.subarray(0, zero === -1 ? 48 : zero).toString('ascii');
    const count = frame.readUInt16LE(100);
    if (id < BASKET_ID_BASE || id >= BASKET_ID_BASE + MAX_BASKETS || count > MAX_BASKET_LEGS || frame.readUInt16LE(102) || !step || minimum < 1 ||
        maximum < minimum || (maximum - minimum) % step || (maximum - minimum) / step + 1 > 100001 ||
        reference < minimum || reference > maximum || (reference - minimum) % step)
        throw Error('Malformed basket definition');
    const legs = Array.from({ length: count }, (_, i) => ({
        instrument_id: frame.readUInt32LE(104 + i * 8), quantity: frame.readUInt32LE(108 + i * 8),
    }));
    const group = validateGroup({ name, legs }, baseProducts);
    if (frame.subarray(104 + count * 8, 232).some(value => value !== 0)) throw Error('Malformed basket leg padding');
    for (let i = 0; i < count; ++i)
        if (legs[i].instrument_id !== group.legs[i].instrument_id) throw Error('Noncanonical basket definition');
    return Object.freeze({ id, kind: 'basket', name: group.name, symbol: `BASKET-${id - BASKET_ID_BASE + 1}`,
        legs: group.legs, tick_size_ticks: step, min_price_ticks: minimum, max_price_ticks: maximum,
        reference_price_ticks: reference, min_price: minimum / 10000, max_price: maximum / 10000 });
}

// A control connection has no order flow. Engine acknowledgement is the only
// authority for a newly tradable ID, and reconnects enumerate its registry.
export class BasketControl {
    constructor({ host, port, baseProducts, onCatalog = () => {}, onLoss = () => {}, reconnectMs = 500, syncTimeoutMs = 15000 }) {
        Object.assign(this, { host, port, baseProducts, onCatalog, onLoss, reconnectMs, syncTimeoutMs });
        this.ready = false; this.groups = new Map(); this.pending = new Map(); this.sequence = 0n;
        this.closed = false; this.socket = null; this.retry = null;
    }
    catalog() { return [...this.baseProducts, ...this.groups.values()]; }
    start() {
        if (this.closed) return;
        const socket = net.createConnection({ host: this.host, port: this.port }); this.socket = socket;
        socket.setNoDelay(true);
        const syncTimer = setTimeout(() => { this.ready = false; socket.destroy(); }, this.syncTimeoutMs); syncTimer.unref();
        let rx = Buffer.alloc(0), staged = new Map(), syncId;
        socket.on('connect', () => {
            syncId = ++this.sequence;
            const frame = Buffer.alloc(12); frame.writeUInt16LE(12); frame[2] = 5;
            frame.writeBigUInt64LE(syncId, 4); socket.write(frame);
        });
        socket.on('error', () => { this.ready = false; });
        socket.on('end', () => { this.ready = false; });
        socket.on('data', bytes => {
            try {
                rx = Buffer.concat([rx, bytes]);
                if (rx.length > 65536) throw Error('Control response buffer exceeded');
                while (rx.length >= 4) {
                    const length = rx.readUInt16LE(0), type = rx[2];
                    if (length !== 236 || ![14, 15].includes(type) || rx[3]) throw Error('Malformed control response');
                    if (rx.length < length) break;
                    const frame = rx.subarray(0, length), requestId = frame.readBigUInt64LE(4), reason = frame.readBigUInt64LE(12);
                    if (requestId === syncId && !this.ready) {
                        if (reason) throw Error('Engine registry synchronization failed');
                        if (type === 15) {
                            if (frame.subarray(20).some(value => value !== 0)) throw Error('Malformed registry synchronization terminator');
                            clearTimeout(syncTimer);
                            this.groups = staged; this.ready = true; this.onCatalog(this.catalog(), true);
                        } else {
                            const product = decodeGroup(frame, this.baseProducts);
                            if (staged.size >= MAX_BASKETS || staged.has(product.id)) throw Error('Invalid registry synchronization');
                            staged.set(product.id, product);
                        }
                    } else {
                        const pending = this.pending.get(requestId.toString());
                        if (!pending || type !== 14) throw Error('Unknown basket control response');
                        if (reason) {
                            clearTimeout(pending.timer); this.pending.delete(requestId.toString());
                            pending.reject(Error(reason === 10n ? 'Basket registry is full (maximum 8 groups)' : 'Engine rejected the basket definition'));
                        }
                        else {
                            const product = decodeGroup(frame, this.baseProducts);
                            if (!this.groups.has(product.id) && this.groups.size >= MAX_BASKETS) throw Error('Basket registry limit exceeded');
                            if (JSON.stringify(product.legs) !== JSON.stringify(pending.group.legs)) throw Error('Mismatched basket acknowledgement');
                            const existing = this.groups.get(product.id);
                            if (existing && JSON.stringify(existing) !== JSON.stringify(product)) throw Error('Basket definition changed');
                            clearTimeout(pending.timer); this.pending.delete(requestId.toString());
                            this.groups.set(product.id, product); pending.resolve(product); this.onCatalog(this.catalog(), true);
                        }
                    }
                    rx = rx.subarray(length);
                }
            } catch { this.ready = false; socket.destroy(); }
        });
        socket.on('close', () => {
            clearTimeout(syncTimer);
            this.ready = false; this.groups.clear();
            for (const pending of this.pending.values()) {
                clearTimeout(pending.timer);
                pending.reject(Error('Basket control connection lost; creation may have completed. Reconnect to synchronize.'));
            }
            this.pending.clear(); this.onCatalog(this.catalog(), false);
            if (!this.closed) {
                this.onLoss(); this.retry = setTimeout(() => this.start(), this.reconnectMs);
                this.retry.unref();
            }
        });
    }
    create(request) {
        const group = validateGroup(request, this.baseProducts);
        if (!this.ready || this.socket?.destroyed) return Promise.reject(Error('Basket registry is synchronizing; try again shortly'));
        if (this.pending.size >= 64 || this.socket.writableLength > 65536)
            return Promise.reject(Error('Too many pending basket creations; try again shortly'));
        const requestId = ++this.sequence;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => { this.ready = false; this.socket.destroy(); }, 15000); timer.unref();
            this.pending.set(requestId.toString(), { resolve, reject, timer, group });
            this.socket.write(encodeGroup(requestId, group));
        });
    }
    close() {
        this.closed = true; this.ready = false; clearTimeout(this.retry); this.socket?.destroy();
    }
}
