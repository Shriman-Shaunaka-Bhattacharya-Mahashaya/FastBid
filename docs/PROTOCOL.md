# Protocol and ownership

## TCP v1

All integers use little-endian encoding. The build rejects big-endian hosts. There is no negotiation, TLS, authentication or compression on the engine socket. Each message starts with a 4-byte header: `uint16 length` including header, `uint8 type`, `uint8 reserved=0`. Invalid length/type/reserved closes the connection immediately. Parsing handles split and coalesced frames. TCP client timestamps are ignored; arrival timestamps are assigned using server monotonic time.

| Request | Type | Bytes | Payload fields in order |
| --- | --- | --- | --- |
| NEW | 1 | 38 | client_order_id u64, instrument_id u32, side u8 (BUY=1, SELL=2), order_type u8 (LIMIT=1), price u64, quantity u32, timestamp u64 |
| CANCEL | 2 | 16 | exchange order_id u64, instrument_id u32 |

Price is dollars multiplied by 10,000; quantity must be nonzero. Instrument must be 1. Client IDs are echoed but not deduplicated. IDs need not be unique across sessions. The server assigns monotonic exchange IDs within the process. Pool exhaustion rejects even if an order could fully match existing liquidity.

| Response | Type | Bytes | Payload fields in order |
| --- | --- | --- | --- |
| ACK | 10 | 20 | exchange order_id u64, client_order_id u64 |
| FILL | 11 | 36 | order_id u64, trade_id u64, fill_price u64, fill_quantity u32, remaining_quantity u32 |
| CANCEL_ACK | 12 | 20 | order_id u64, original client_order_id u64 |
| REJECT | 13 | 28 | requested exchange order_id u64 (0 for NEW), client_order_id u64, reason u64 |

Reject reasons: 1 price outside band; 2 pool exhausted; 3 order missing; 4 instrument mismatch; 5 invalid quantity/side/type; 7 wrong owner session; 8 ID space exhausted. CANCEL has no client_order_id, so its rejection echoes zero for that field. Execution uses the resting order's price and FIFO at equal prices. Both counterparties receive a fill with the same trade ID. ACK indicates validation/admission, not durability. Successful socket writes do not prove application-level receipt.

The internal `connection_fd` field is a session token, not an OS descriptor. Reuse of file descriptors cannot expose earlier reports. Ownership survives neither reconnect nor restart. Slow TCP clients are disconnected at the fixed output bound; requests already admitted can still execute and resting orders remain.

## Market data v1

UDP datagrams are explicit packed layouts, never internal engine structs. Common 16-byte header: magic u32 `0x31444246` (bytes `FBD1`), version u16 `1`, kind u16, sequence u64. Sequence increases for every publisher send attempt, including dropped sends. No private session or client IDs are included.

**Trade (kind 1, 40 bytes):** header; trade_id u64 at offset 16; price u64 at 24; quantity u32 at 32; aggressor side u8 at 36; three reserved zero bytes. One datagram per trade, despite two private fills.

**Snapshot (kind 2, 384 bytes):** header; bid_count u32 at 16; ask_count u32 at 20; ten bid entries at 24; ten ask entries at 184. Each entry is price u64 followed by aggregate quantity u64. Bids descend, asks ascend. Unused entries are zero. Metrics are u64 fields: p50 ns at 344, p99 ns at 352, processed requests/sec at 360, sample count at 368, cumulative diagnostic sample drops at 376. Counts never exceed 10. Empty metric windows have sample count zero.

The engine copies depth only at completed-request boundaries into a separate bounded SPSC queue. IO sends only newly received snapshots, so a stalled engine does not appear fresh. Snapshot copying is excluded from measured request latency. Outbound-pressure pauses and scheduling interruptions are included. Percentiles use floor-index ranks over retained samples; dropped samples can bias latency estimates. No network round-trip latency is reported. UDP send failures are counted in server logs.

The bridge marks data stale after two seconds without a snapshot and rejects non-increasing sequence numbers until two seconds of inactivity, permitting engine restarts. This is display recovery, not a lossless market-data recovery protocol. Process epoch/session recovery must be added before integrating external trading systems.

## WebSocket bridge

Every browser has its own TCP engine connection. `ready` means that TCP connection is open. JSON commands:

```json
{"type":"new_order","side":"BUY","price":10.0001,"quantity":100}
{"type":"cancel","order_id":"123"}
```

Exchange IDs are JSON strings to avoid JavaScript precision loss. The bridge returns `execution_report` (ACK/FILL/CANCEL_ACK/REJECT) to the owner and broadcasts `book_snapshot` (including `stats`) and `trade`. `error` means a bridge request was not dispatched; `stale` means the data feed has timed out. A session loss after dispatch has ambiguous acceptance. The dashboard reconnects but never automatically resubmits orders.

Bridge limits: 256 clients, 4 KiB inbound messages, 64 KiB buffered WS output, 100 commands/sec/client with a 100-command burst. Origin filtering is browser protection only; callers can forge Origin outside a browser. Use authenticated private access for remote use.
