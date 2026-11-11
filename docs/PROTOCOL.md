# Protocol and ownership

## TCP v1

All integers use little-endian encoding. The build rejects big-endian hosts. There is no negotiation, TLS, authentication or compression on the engine socket. Each message starts with a 4-byte header: `uint16 length` including header, `uint8 type`, `uint8 reserved=0`. Invalid length/type/reserved closes the connection immediately. Parsing handles split and coalesced frames. TCP client timestamps are ignored; arrival timestamps are assigned using server monotonic time.

| Request | Type | Bytes | Payload fields in order |
| --- | --- | --- | --- |
| NEW | 1 | 38 | client_order_id u64, instrument_id u32, side u8 (BUY=1, SELL=2), order_type u8 (LIMIT=1), price u64, quantity u32, timestamp u64 |
| CANCEL | 2 | 16 | exchange order_id u64, instrument_id u32 |

Price is dollars multiplied by 10,000; quantity must be nonzero. Instrument must be an ID from config/products.json (defaults: 1 Apples, 2 Bananas, 3 Oranges). Orders match only within that product. CANCEL must supply the original product; MODIFY cannot move an order between products. Client IDs are echoed but not deduplicated. IDs need not be unique across sessions. The server assigns monotonic exchange IDs within the process. Pool exhaustion rejects even if an order could fully match existing liquidity.

| Response | Type | Bytes | Payload fields in order |
| --- | --- | --- | --- |
| ACK | 10 | 20 | exchange order_id u64, client_order_id u64 |
| FILL | 11 | 36 | order_id u64, trade_id u64, fill_price u64, fill_quantity u32, remaining_quantity u32 |
| CANCEL_ACK | 12 | 20 | order_id u64, original client_order_id u64 |
| REJECT | 13 | 28 | requested exchange order_id u64 (0 for NEW), client_order_id u64, reason u64 |

Reject reasons: 1 price outside band; 2 pool exhausted; 3 order missing; 4 unknown instrument; 5 invalid quantity/side/type; 7 wrong owner session; 8 ID space exhausted. CANCEL has no client_order_id, so its rejection echoes zero for that field. Execution uses the resting order's price and FIFO at equal prices. Both counterparties receive a fill with the same trade ID. ACK indicates validation/admission, not durability. Successful socket writes do not prove application-level receipt.

The internal `connection_fd` field is a session token, not an OS descriptor. Reuse of file descriptors cannot expose earlier reports. Ownership survives neither reconnect nor restart. Slow TCP clients are disconnected at the fixed output bound; requests already admitted can still execute and resting orders remain.

## Market data v2

UDP packets are explicit little-endian packed layouts. The 24-byte common header contains magic u32 `0x31444246` (bytes `FBD1`), version u16 `2`, kind u16, global packet sequence u64, instrument_id u32 at offset 16, and reserved u32 `0` at offset 20. Receivers must validate version, size, reserved field and catalog product ID. Version-1 decoders must be upgraded; the extra product fields move every subsequent offset by eight bytes.

**Trade (kind 1, 48 bytes):** header; trade_id u64 at 24; price u64 at 32; quantity u32 at 40; aggressor side u8 at 44; three reserved zero bytes. One datagram per match, despite two participant reports. Exchange order IDs and trade IDs are globally unique across the product books within the process.

**Snapshot (kind 2, 392 bytes):** header; bid_count u32 at 24; ask_count u32 at 28; ten bid entries at 32; ten ask entries at 192. Each entry has price u64 and aggregate quantity u64. Bids descend, asks ascend, and unused entries are zero. Counts never exceed ten. Metrics are u64 fields: p50 ns at 352, p99 ns at 360, processed requests/sec at 368, sample count at 376 and cumulative sample drops at 384. These metrics cover the whole engine, not only the packet's product. A zero sample count means no measured window.

At completed-request boundaries, the engine copies a separate depth snapshot for every configured product into a bounded snapshot queue, approximately every 50 ms. IO retains pending snapshots per product and publishes only newly received copies. An empty product still gets an empty snapshot. Public packets contain no session/client IDs or compiler padding.

The bridge caches snapshots and tracks freshness independently per product. It sends product-tagged stale notifications when a product has no fresh snapshot for two seconds; overall health requires all configured products to be fresh. The global sequence increases per send attempt, including failed sends. Non-increasing packets are rejected while the snapshot feed is active; two seconds of inactivity permits a restarted engine's reset sequence.

Full top-ten snapshots repair displayed depth after packet loss. They do not provide historical trade replay or a lossless recovery protocol. A stall or backpressure can delay snapshots. Snapshot copying is excluded from engine request latency; report-pressure pauses and scheduling interruptions are included. Sample drops can bias percentiles. No network round-trip latency is reported.

## WebSocket bridge

Every browser has its own TCP engine connection, shared across its selected products. On connection the bridge sends `catalog` with product definitions. `instrument_id` selects a product for NEW/CANCEL; omission defaults to the first catalog entry for legacy clients. Explicit null, unknown IDs and non-integer IDs are rejected before dispatch. UI requests always include it. `ready` means that TCP connection is open. JSON commands:

```json
{"type":"new_order","instrument_id":2,"side":"BUY","price":10.0001,"quantity":100}
{"type":"cancel","instrument_id":2,"order_id":"123"}
```

Execution reports, trades, snapshots and stale notifications carry `instrument_id`. The binary TCP response layout remains unchanged: the bridge correlates ACKs with pending requests and tracks product ownership for fills/cancels. Cancel request product IDs are queued for response correlation, including rejections. Wrong-product cancel/modify on a known product returns order-not-found (reason 3) without changing the original. Exchange IDs are JSON strings to avoid JavaScript precision loss. The bridge returns `execution_report` (ACK/FILL/CANCEL_ACK/REJECT) to the owner and broadcasts `book_snapshot` (including `stats`) and `trade`. `error` means a bridge request was not dispatched; `stale` means the data feed has timed out. A session loss after dispatch has ambiguous acceptance. The dashboard reconnects but never automatically resubmits orders.

Bridge limits: 256 clients, 4 KiB inbound messages, 64 KiB buffered WS output, 100 commands/sec/client with a 100-command burst. Origin filtering is browser protection only; callers can forge Origin outside a browser. Use authenticated private access for remote use.
