# FastBid

FastBid is a C++20 limit order book matching engine with a binary TCP gateway, a Node.js WebSocket bridge, and a React dashboard. It provides a complete **local paper-trading environment**: submit simulated limit orders, watch them match, inspect the book, and cancel your session's remaining orders.

The focus is predictable order processing: preallocated memory, one owner of the book, bounded queues, price-time priority, and measured latency. The default product catalog contains Apples, Bananas, and Oranges. Orders match only with the same product. The project is not connected to an exchange and does not manage real funds, balances, or positions.

## Contents

- [Capabilities and scope](#capabilities-and-scope)
- [Quick start: local paper trading](#quick-start-local-paper-trading)
- [Products and catalog configuration](#products-and-catalog-configuration)
- [Create and trade baskets](#create-and-trade-baskets)
- [Try a complete trading workflow](#try-a-complete-trading-workflow)
- [Architecture](#architecture)
- [Matching rules and data structures](#matching-rules-and-data-structures)
- [Technology choices](#technology-choices)
- [Project layout](#project-layout)
- [Build and run from source](#build-and-run-from-source)
- [Configuration](#configuration)
- [Protocols and overload behavior](#protocols-and-overload-behavior)
- [Continuous integration](#continuous-integration)
- [Testing and performance measurement](#testing-and-performance-measurement)
- [Operations and troubleshooting](#operations-and-troubleshooting)
- [Limitations and future work](#limitations-and-future-work)

## Capabilities and scope

| Area | Implemented behavior |
| --- | --- |
| Matching | Independent base-product and basket books; limit BUY/SELL orders; best price first, then arrival order |
| Execution | Partial fills, multiple fills, execution at the resting order's price |
| Cancellation | The current owner session can cancel its resting orders |
| Baskets | Runtime creation of immutable weighted groups, shared catalogs and whole-basket trading |
| Modification | Internal cancel-and-replace implementation; not exposed over TCP or the dashboard |
| Memory | Fixed order pool, price array, intrusive order index, bounded SPSC queues |
| TCP | Binary NEW/CANCEL requests; correlated ACK/FILL/CANCEL_ACK/REJECT responses |
| Market data | Versioned trade packets and full top-10 depth snapshots |
| Dashboard | Order entry, open orders, cancellation, trades, depth, latency history, stale-feed indication |
| Local operation | Docker-based paper stack, startup helper, readiness endpoint, stop/reset commands |
| Validation | Unit tests, randomized reference model, sanitizer checks, socket/bridge integration, paper-stack smoke test |

SPSC means single-producer/single-consumer: exactly one thread writes a queue and exactly one other thread reads it. The matching path performs no heap allocation after startup. This does not mean the entire application never allocates: connection setup, JavaScript processing, and build tools do allocate.

## Quick start: local paper trading

### Prerequisites

- Windows: Docker Desktop running in Linux-container mode and PowerShell.
- Linux: Docker Engine and the Docker Compose plugin.
- A free local TCP port `8080`.
- Internet access for the first image/dependency build.

The container workflow does not require a locally installed C++ compiler, Node.js, or a separately opened WSL terminal. Run commands from the project root unless stated otherwise.

### Start

On Windows:

```powershell
.\paper.ps1 start
```

On either platform, the equivalent Docker command is:

```bash
docker compose -f compose.paper.yml up -d --build --wait --wait-timeout 120
```

Open **http://localhost:8080**. Wait for the dashboard to show **LIVE**. The first build takes longer because it downloads dependencies and compiles the engine and frontend.

The dashboard container serves the built web page, WebSockets, and health endpoint. The engine shares its isolated network namespace [the containers' network interfaces and ports], so the bridge reaches TCP and UDP over loopback. Only `127.0.0.1:8080` is published to the host. No host-network support or multicast routing is required for this paper stack.

### Manage the session

```powershell
.\paper.ps1 status  # Show containers and health
.\paper.ps1 logs    # Show recent diagnostics
.\paper.ps1 reset   # Discard all simulated orders and start an empty book
.\paper.ps1 stop    # Stop and remove containers
```

Direct Docker equivalents:

```bash
docker compose -f compose.paper.yml ps
docker compose -f compose.paper.yml logs --tail 100
docker compose -f compose.paper.yml down
```

To reset on Linux, run `down`, then the startup command. The PowerShell `reset` action reuses existing images; run `start` after source changes to rebuild them. If PowerShell blocks script execution, use the direct Docker command.

**State is volatile [kept only in process memory].** Stop, reset, or an engine restart discards the book. There is no recovery from disk.

## Products and catalog configuration

Use the **Product** selector above the dashboard to choose what to buy or sell. The selected product controls order entry, depth, recent trades, and the visible open-order list. Switching products preserves your orders in every other product. Fills and cancellation reports remain attached to their original product even if you switch while waiting for a reply. A client can trade multiple products within the same session.

| Product ID | Symbol | Product | Default price band per unit |
| --- | --- | --- | --- |
| 1 | APPLE | Apples | $5-$15 |
| 2 | BANANA | Bananas | $5-$15 |
| 3 | ORANGE | Oranges | $5-$15 |

A BUY for Apples never matches a SELL for Bananas, even at the same price. To try this, submit an Apples BUY in one tab and a Bananas SELL in another; both should remain open. Select Bananas in the first tab and submit a BUY to match that seller. Return to Apples to manage the original bid. Cancels must identify the original product and owning session; changing the selector does not transfer orders between books.

[`config/products.json`](config/products.json) is the source of truth for IDs, names, symbols and price bands. CMake generates fixed engine configuration from it; the bridge reads the same file and sends the catalog to browsers. Price fields in this file are integer ticks (dollars multiplied by 10,000). IDs must be distinct positive 32-bit integers. Each product needs a valid reference price within its band; the band can contain at most 100,001 integer levels. The current implementation supports 1-16 configured products, subject to available memory. IDs should remain stable when changing names or symbols.

Add or edit an entry, then rebuild **both** services with `paper.ps1 start` or the Compose startup command. Catalog changes are not hot-reloaded. Recreating the engine discards the current simulated book, so finish or intentionally discard your session before applying a catalog update. The supplied browser/isolation scenarios use the default three-product catalog.

One matching thread serializes all products, sharing the existing order pool, inbound queue, outbound queue and global order/trade ID counters. Each product has separate price levels, occupancy bitmap and order index. A full shared pool can reject requests for any product. A pending sweep retains priority across the shared input stream. More products reserve more book/index memory; they do not create a matching thread per product. Latency and throughput statistics remain engine-wide, while depth, trades and freshness are product-specific. The bridge's health endpoint requires fresh snapshots for every configured product.

Market-data packets now use version 2 and include product IDs. Update custom UDP subscribers alongside the engine/bridge. Binary TCP NEW and CANCEL messages retain their existing sizes and already include `instrument_id`; see [protocol details](docs/PROTOCOL.md).

## Create and trade baskets

A basket is a tradable group containing fixed quantities of different base products. For example, **Fruit Mix** can contain **2 Apples + 3 Bananas**. One order unit is one entire basket. A fill of three units represents **6 Apples + 9 Bananas together**. The entered price is the **total price per basket**, not a price per individual share.

### Browser workflow

1. Open the dashboard and wait until basket creation is available. This requires the bridge to synchronize with the engine's basket registry.
2. In **Create a Product Basket**, enter a name and choose at least two different base products with the desired shares per basket.
3. Click **Create basket**. Wait for the engine acknowledgment; only then is the basket added to the shared selector.
4. Select the basket in another tab. Its description shows the recipe, total-price range and allowed increment.
5. In the first tab, BUY three baskets at the reference total price. In the second tab, SELL one identical basket at that price.
6. One basket trades, leaving two on the buyer's order. Cancel the remainder as with a base-product order.

Basket definitions are shared by all connected clients. The name is a label; the **exact composition** identifies the market. Reordering the same legs reuses the same basket ID and original name. `2 Apples + 4 Bananas` is different from `1 Apple + 2 Bananas`: quantities are not divided by a common factor. Two different recipes with the same name still have separate books. Definitions cannot be edited or deleted during a session; creating a new recipe consumes a new slot.

Baskets trade against identical basket orders in their own books. They do **not** split into individual product orders, consume liquidity in underlying books, check stock ownership, or transfer cash/inventory in an account ledger. Partial fills are allowed in whole basket units, so constituent ratios are never partially filled within one unit. Creating a group does not purchase or reserve its ingredients.

### Validation and price increments

At most **eight** basket definitions can exist per engine process. Each needs **2-16 distinct base products**, each with **1-1,000 integer shares**, with at most **10,000 total shares**. Nested baskets, repeated legs, unknown products, zero/fractional quantities and oversized definitions are rejected. Names contain 1-48 trimmed printable ASCII characters. Once the eight slots are full, new recipes are rejected; existing recipes remain tradable and repeated creation still returns their existing ID. Definitions disappear when the engine restarts.

The basket price range is derived from the weighted sums of constituent minimum/maximum prices. To retain bounded array storage, its increment in integer ticks is `max(1, ceil((weighted_max - weighted_min) / (book_level_capacity - 1)))` when the capacity exceeds one. The highest allowed price and reference are rounded down to this grid anchored at the weighted minimum. A one-level book supports a constant-price basket. The UI exposes the actual range/increment and the bridge and engine both reject off-grid prices.

For the default `2 Apples + 3 Bananas` recipe, the range is **$25-$75**, reference **$50**, and increment **$0.0005**. A $50.0001 order is invalid; $50.0005 is valid. These are configured simulation price bands, not live valuations. Base-product increments remain $0.0001.

### Ownership, recovery and performance

The matching thread owns registration and assigns IDs from `2147483648` upward. It reserves eight extra book slots at startup and initializes a new book between completed order requests. Registration can briefly delay order processing while a book is initialized, but ordinary order messages and fill events retain their compact sizes. The shared order pool and globally unique exchange/trade IDs also cover baskets. A full pool can affect any instrument.

A dedicated bridge control connection registers and synchronizes definitions separately from order flow. After a bridge-only restart, definitions are fetched from the still-running engine with their original IDs. Losing the control connection disables group creation/trading and closes existing browser sessions until they reconnect; old session orders still follow the documented reconnect limitation. An engine restart clears both definitions and orders, so stale basket IDs cannot be reused blindly. Native control clients can explicitly synchronize; the browser bridge discovers its own registrations immediately and other control clients' registrations on its next synchronization.

Budget roughly **290 MiB for core reserved address space** in the default process including eight potential basket books, plus network/runtime overhead; residency grows as books are initialized. Configuring more base products increases that reservation. Local image recreation clears simulated state, so finish a session before applying a rebuild. Use the existing `paper.ps1 start` command to load this feature.

## Try a complete trading workflow

1. Open the dashboard in two browser tabs. Each tab receives a separate trading session. Select **Apples** in both tabs.
2. In tab A, submit **BUY**, price **10**, quantity **100**. The order should appear in **My Open Orders** and bid depth.
3. In tab B, submit **SELL**, price **10**, quantity **40**.
4. The engine trades 40 units at $10. Both tabs receive their own fill report. Tab A has 60 units left.
5. In tab A, click **Cancel**. Wait for `CANCEL_ACK`; the remaining order disappears.

Each product book begins empty, so an order will rest until a compatible opposite order for that product arrives. There is no automated market maker supplying liquidity.

Prices in the UI are dollar values, not internal integer ticks. The default catalog gives each product a $5 through $15 per-unit price band, with up to four decimal places. Quantity must be a positive whole number.

Keep the tab open while managing its orders. Refreshing or reconnecting creates a new session; it cannot recover cancellation access to the old session's orders. Reset the paper stack to clear those orders. An interrupted connection can leave acceptance uncertain, so the dashboard never automatically resubmits requests.

## Architecture

```mermaid
flowchart LR
    UI[React dashboard] <-->|WebSocket JSON| Bridge[Node.js bridge]
    Bridge <-->|Binary TCP requests and reports| IO[C++ IO thread]
    IO -->|Inbound SPSC queue| Engine[Matching thread]
    Engine --> Book[Order pool and book]
    Engine -->|Outbound SPSC queue| IO
    Engine -->|Snapshot queue| IO
    Engine -->|Latency sample queue| IO
    IO -->|UDP trades and depth| Bridge
```

### Responsibilities

**Dashboard:** collects user input and presents acknowledgments, fills, open orders, book depth, trades, and diagnostics. The browser does not decide whether an order matched.

**Bridge:** translates browser JSON into the compact binary order protocol, creates one engine TCP connection per browser, routes private reports to their owner, and converts public UDP packets into browser messages. In the paper image it also serves built frontend files.

**IO thread:** the C++ main thread accepts TCP connections using Linux `epoll`, parses complete frames, queues requests, sends reports, publishes market data, and computes diagnostic percentiles. It owns the sockets.

**Matching thread:** owns the shared order pool and all product books. It validates requests, matches orders, and emits events. No other thread directly mutates the book.

Additional bounded queues transfer engine-owned snapshots and latency samples to IO. The separation prevents the networking and monitoring code from reading mutable book state concurrently.

## Matching rules and data structures

### Price-time priority

A buy matches the lowest eligible sell price. A sell matches the highest eligible buy price. At the same price, the oldest order fills first: FIFO [first in, first out]. A trade executes at the resting order's price, which may improve on the incoming order's limit.

For example, a resting SELL at $9.90 can match an incoming BUY limited to $10.00, and the trade price is $9.90. Unfilled incoming quantity rests at its own limit price.

### Storage

| Structure | Purpose and tradeoff |
| --- | --- |
| Flat price array | Direct lookup using `price - minimum_price`; reserves memory for empty prices too |
| Intrusive FIFO lists | Each order contains links to its neighbors; avoids allocating separate list nodes |
| Object pool | Reserves order slots at startup and recycles them through a free list |
| Intrusive hash index | Finds an order by exchange ID without allocating map nodes; collisions still require chain traversal |
| Occupancy bitmap | One bit per price indicates whether it contains orders, allowing scans to skip empty groups |
| SPSC ring buffers | Transfer fixed-size values between threads without a mutex; each queue has exactly one producer and consumer |

Each product book uses one price-level array. A correctly matched resting book never has crossing buy/sell prices, so opposite resting sides do not need to coexist at the same price. Prices are fixed-point integers: `$10.0001` is represented as `100001`, avoiding floating-point comparison errors.

Direct level lookup and pool slot allocation are constant-time operations. Order-ID lookup is expected constant time but can degrade with collisions. Removing a known list node is constant time; locating a new best price may scan bitmap words. Matching an order that fills many resting orders necessarily performs work for each fill.

## Technology choices

| Technology | Use in this project |
| --- | --- |
| C++20 | Explicit memory layout, fixed-capacity structures, atomics, bit operations, and Linux system calls |
| Linux `epoll` and nonblocking sockets | Manage multiple connections in one IO loop |
| TCP | Ordered byte stream for requests and private execution reports |
| UDP | Best-effort public trades/depth; multicast for native configuration, loopback unicast for paper containers |
| Node.js and `ws` | Browser WebSocket support kept outside the matching thread |
| React 18 | Dashboard state and rendering |
| Vite 8.3.4 / React plugin 6.1.2 | Frontend development and production asset builds; exact tool versions are in the package files |
| CMake / CTest / GoogleTest | C++ builds, test execution, and unit assertions; GoogleTest source revision is pinned |
| Python 3 | Real-process TCP integration checks |
| Docker / Compose | Repeatable build/runtime environments and multi-service lifecycle |
| PowerShell | Windows helper for paper-stack commands |

There is no database, `io_uring` integration, kernel-bypass implementation, or C++ WebSocket server in the current runtime.

## Project layout

```text
FastBid/
  README.md
  .github/workflows/ci.yml       Automated build, sanitizer, integration and browser checks
  config/products.json          Shared product identities and integer price bands
  compose.paper.yml             Local complete paper-trading stack
  paper.ps1                     Windows lifecycle helper
  docs/PROTOCOL.md              Binary layouts and message semantics
  server/
    CMakeLists.txt
    Dockerfile
    docker-compose.yml          Alternative native-Linux host-network stack
    src/
      main.cpp                  Startup, thread ownership, telemetry, shutdown
      core/                     Orders, price levels, book, matching engine
      memory/                   Object pool and SPSC queue
      network/                  TCP gateway and UDP market data
      platform/                 CPU affinity and counter timing
      instrumentation/          Latency sample collection
    tests/                      Unit, reference-model, and socket tests
    benchmarks/                 Core benchmark and C++ allocation guard
  client/
    src/                        React dashboard
    ws_bridge.js                HTTP/WebSocket/TCP/UDP bridge
    Dockerfile.paper            Builds and serves the complete dashboard
    Dockerfile.bridge           Bridge-only image
    tests/                      Bridge integration and paper-stack smoke test
  deploy/
    setup_oracle_vm.sh          Ubuntu host provisioning helper
```

## Build and run from source

### C++ engine

Use Linux or Ubuntu under WSL. Required tools: a C++20 compiler, CMake 3.20+, Git, and Python 3 for tests. On Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake git python3
```

From the project root:

```bash
cmake -S server -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
./build/release/fastbid
```

CMake fetches GoogleTest during the first test-enabled configuration. For an offline checkout, pass `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/absolute/path/to/googletest`. Windows-native engine builds are rejected because the networking layer requires Linux.

### Bridge and frontend development

Use Node.js 22.12+ or 20.19+ for the current frontend toolchain. With the engine running, open a second terminal:

```bash
cd client
npm ci
node ws_bridge.js
```

Open a third terminal:

```bash
cd client
npm run dev
```

Visit the Vite address, normally `http://localhost:5173`. If Vite chooses a different port, configure the bridge's allowed origins to match. `npm ci` installs the lockfile versions. `npm run build` creates `client/dist/`, which the bridge can serve directly on port 8080; for that direct browser URL, also set its allowed origins to include `http://localhost:8080`.

Avoid running the paper stack and source bridge on the same host port simultaneously. In WSL, running engine, bridge, and development frontend together inside Linux avoids cross-environment UDP routing assumptions.

### Alternative native-Linux containers

```bash
docker compose -f server/docker-compose.yml up --build -d
```

This alternative uses host networking and multicast, and builds the **bridge-only** image. It does not package the React dashboard: run the development frontend separately. Prefer `compose.paper.yml` for a complete local workflow, especially on Docker Desktop. Do not launch both stacks on the same ports.

## Configuration

Environment variables [settings supplied when starting a process] configure the following values:

| Variable | Default outside paper Compose | Meaning |
| --- | --- | --- |
| `FASTBID_PAPER_PORT` | `8080` | Published paper-stack host port and matching allowed browser origins |
| `FASTBID_TCP_HOST` | `127.0.0.1` | Engine bind IPv4; bridge's engine destination |
| `FASTBID_TCP_PORT` | `9000` | Binary order entry |
| `FASTBID_UDP_MCAST_ADDR` | `239.255.0.1` | Engine UDP destination; bridge multicast membership |
| `FASTBID_UDP_MCAST_PORT` | `5000` | UDP market-data port |
| `FASTBID_WS_HOST` | `127.0.0.1` | Bridge HTTP/WebSocket bind address |
| `FASTBID_WS_PORT` | `8080` | Bridge HTTP/WebSocket port |
| `FASTBID_ALLOWED_ORIGINS` | localhost and 127.0.0.1 on port 5173 | Comma-separated allowed browser origins |
| `FASTBID_LOCK_MEMORY` | `0` | `1` attempts to lock existing engine mappings in RAM |
| `VITE_WS_URL` | Development: `ws://localhost:8080`; built UI: current page's host | Frontend build-time WebSocket URL override |

Paper Compose sets the engine's UDP destination to `127.0.0.1`, binds the bridge to all interfaces inside its container, and permits the two local port-8080 browser origins. The published host port remains loopback-only. Set frontend build-time variables before building the frontend; changing them in a running container does not rewrite compiled assets.

Current limits:

| Setting | Value |
| --- | --- |
| Products | IDs `1`, `2`, `3`; shared catalog; up to 16 configured base products plus eight runtime baskets |
| Price band | `50000` to `150000` ticks inclusive, equivalent to $5-$15 |
| Reference price | `100000` ticks, equivalent to $10 |
| Price levels | Up to `100001` per product |
| Order pool | `1048576` shared slots across all products, including the pending aggressor |
| Inbound/outbound queues | `65536` slots each, `65535` usable |
| TCP clients | `1024` |
| Per-client TCP receive/send buffers | 8 KiB / 64 KiB |
| Browser clients | `256` |
| Browser request size | 4 KiB |
| Bridge command rate | 100/second/client, with a 100-command burst |
| Snapshot depth and interval | Top 10 per side, approximately 50 ms when engine progress allows |

Core capacities are compiled into the program; they are not all environment-configurable. Budget roughly 290 MiB of reserved core address space with the default three-product catalog and eight basket slots for engine/queue state, plus connection buffers, kernel socket buffers, runtime overhead, and frontend services. Busy polling can use substantial CPU even when idle. CPU affinity uses the process's allowed CPU mask and normal scheduling. Optional memory locking does not lock future allocations and can fail without sufficient privileges.

## Protocols and overload behavior

The engine accepts little-endian binary TCP frames [messages with a defined byte layout]. NEW is 38 bytes; CANCEL is 16 bytes. Responses are ACK, FILL, CANCEL_ACK, and REJECT. Each frame includes its length and type. See [protocol details](docs/PROTOCOL.md) for exact field offsets, reject reasons, and public market-data formats.

The browser uses JSON, for example:

```json
{"type":"new_order","side":"BUY","price":10.0001,"quantity":100}
```

```json
{"type":"cancel","order_id":"123"}
```

Order IDs are strings in browser messages to avoid JavaScript integer precision loss. ACK means accepted by the in-memory engine, not saved durably.

When report capacity is insufficient, matching pauses between fills and retains the active order's priority. Later requests cannot overtake it. Inbound requests remain buffered until space becomes available; eventually TCP flow control slows the sender. Slow consumers exceeding the output bound are disconnected rather than permitted unlimited memory growth. A disconnect can still lose delivery of reports for already executed work.

TCP preserves byte order but not message boundaries; the parser handles split and combined frames. Invalid lengths/types/reserved fields close the connection. Engine validation rejects invalid prices, quantities, types, instruments, and unauthorized cancellation.

Public UDP data is best effort [delivery is not guaranteed]. One product-tagged trade packet is published per match, while each participant receives a private fill. Full top-10 snapshots recover displayed depth after missed packets, but there is no replay for missed trades.

## Continuous integration

[The CI workflow](.github/workflows/ci.yml) runs on every push, pull request, and manual dispatch. Push the workflow with the project to enable these checks in GitHub's **Actions** tab. No deployment or image publication is performed. Required branch-protection checks must be selected separately in repository settings after the first run.

| Job | Checks |
| --- | --- |
| GCC Release | Unit/model/socket tests and a C++ allocation regression guard |
| Clang ASan/UBSan | The same CTest suite with memory/undefined-behavior instrumentation |
| GCC coverage | Instrumented suite plus HTML and Cobertura XML coverage reports |
| ThreadSanitizer | Concurrent queue, sample-transfer and two-thread engine stress tests |
| Node.js 22 and 24 | Locked dependency installation, dashboard build, controlled bridge failure-path tests |
| Engine/bridge integration | Real product and basket protocols, simultaneous recipe creation, partial fills, bridge resynchronization and engine restart invalidation |
| Docker and Chromium | Isolated paper-image build, readiness, socket smoke test and actual dashboard browser tests |

The workflow has read-only repository permissions, pins third-party actions to commit hashes, cancels superseded runs on the same ref, and uses bounded job timeouts. Diagnostic artifacts are restricted to named test reports, coverage files, container logs, and browser results; retention is seven days. The temporary test stack is stopped even when a test fails. Hosted-runner benchmark timing is informational; only allocation/correctness failures fail the benchmark check.

Local validation on 2026-11-11 passed 66 CTest cases in GCC Release, Clang ASan/UBSan and the GCC coverage build; three TSan concurrency cases; 11 Node test results including their parent; six Chromium cases; real bridge integration and the paper smoke test. The C++ report measured 97.1% line coverage and 71.1% branch coverage. Node 24 and the GitHub-hosted environment are configured but await their hosted run.

Coverage reports measure executed code, not proof of correctness. They currently have no percentage gate: inspect uncovered branches and add meaningful cases rather than tests that only reproduce implementation details. Sanitizers and concurrency tests cannot establish correctness for every schedule, architecture or input. The suite does not claim exhaustive testing of all possible executions.

The multi-product update was verified locally on 2026-11-12 with 74 Release cases, 74 Clang ASan/UBSan cases, 14 Node test results, seven Chromium scenarios, real engine/bridge integration, the Docker smoke test, three TSan concurrency checks and a zero-allocation multi-product core benchmark. The earlier coverage percentages above describe the earlier code revision; coverage for this update is collected by the CI coverage job.

The whole-basket extension was verified locally on 2026-11-12 with 93 CTest cases in Release and Clang ASan/UBSan, 30 Node results, nine Chromium scenarios, real engine/bridge recovery integration, the paper smoke test, three TSan concurrency cases plus real basket-control integration under TSan, and zero C++ allocations in the measured base-and-basket benchmark. These results do not update the earlier coverage percentages; CI collects current coverage separately.

### Run the additional checks locally

From `client/`, with a supported Node.js installation:

```bash
npm ci
npm test
npm run test:bridge -- ../build/release/fastbid
npm run test:baskets -- ../build/release/fastbid
```

`npm test` builds the dashboard and exercises a controlled TCP engine and UDP sender. Cases include malformed commands/responses, split replies, origin filtering, rate limiting, oversized messages, static asset restrictions, sequence filtering, feed timeout/recovery and upstream loss.

Browser tests require a clean, running paper stack. To keep an existing session on port 8080 untouched, use a separate project and port. On Linux:

```bash
export FASTBID_PAPER_PORT=18080
export PAPER_BASE_URL=http://127.0.0.1:18080
docker compose -p fastbid-tests -f compose.paper.yml up -d --build --wait --wait-timeout 120
cd client
npx playwright install --with-deps chromium
npm run test:paper -- "$PAPER_BASE_URL"
npm run test:e2e
cd ..
docker compose -p fastbid-tests -f compose.paper.yml down
```

In PowerShell, set the variables with `$env:FASTBID_PAPER_PORT='18080'` and `$env:PAPER_BASE_URL='http://127.0.0.1:18080'`; pass `$env:PAPER_BASE_URL` to the smoke command. Use `npx.cmd playwright install chromium` to install the Windows browser. Browser tests run serially against a shared empty book, with failure traces/screenshots and an HTML report under `client/playwright-report/`. They cover real readiness, input labels, resting/cancelled orders, cross-session partial fills, validation messages, transport recovery and narrow-screen controls. Failed tests may leave simulated orders; recreate only the test stack before rerunning.

For coverage on Linux, install `gcovr` and use a separate GCC build:

```bash
cmake -S server -B build/coverage -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON
cmake --build build/coverage -j
ctest --test-dir build/coverage --output-on-failure
gcovr build/coverage --root . --filter 'server/src/' --object-directory build/coverage --xml build/coverage/coverage.xml --xml-pretty --html-details build/coverage/coverage.html --print-summary
```

Do not combine coverage instrumentation and sanitizers in one build. Keep coverage artifacts separate from other instrumented build directories when collecting results.

## Testing and performance measurement

### Memory and undefined-behavior checks

```bash
cmake -S server -B build/asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_SANITIZERS=ON
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure
```

ASan [AddressSanitizer] detects many memory access errors. UBSan [UndefinedBehaviorSanitizer] detects operations that violate C++ rules. For concurrent-access checking, use a separate build:

```bash
cmake -S server -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DENABLE_TSAN=ON
cmake --build build/tsan -j
./build/tsan/tests/fastbid_tests --gtest_filter=SPSCQueueTest.ConcurrentProducerConsumer:Stats.ConcurrentTransferAccountsForEverySample:EngineConcurrency.*
```

TSan [ThreadSanitizer] checks for data races. Do not combine it with ASan in one build. These tools are diagnostic checks, not proofs of correctness for every possible input.

### Integration checks

The CTest suite includes Python socket tests that launch a real engine on temporary ports. Bridge integration also launches its own engine and bridge; run from `client/` on Linux after `npm ci`:

```bash
node tests/bridge_integration.mjs ../build/release/fastbid
```

For the running Docker paper stack, start with an empty book and run from the project root:

```bash
node client/tests/paper_smoke.mjs
```

The paper smoke test submits simulated orders and checks HTTP assets, health, restricted asset serving, acknowledgments, cancellation, two-session matching, and book snapshots. It requires client dependencies. Do not run it against a session whose test orders you want to preserve.

The suite also checks basket recipes, idempotent creation, limits/overflow, price grids, capacity, whole-unit partial fills, separate underlying books and registry synchronization. It checks cross-product matching/cancel/modify isolation, shared global IDs, product-tagged snapshots, catalog validation and UI switching. It includes forced hash collisions, inclusive/near-maximum price boundaries, sorted/aggregated top-10 snapshots, pool exhaustion, invalid replacement ownership, full-output admission checks, three reference-model seeds (60,000 generated requests), and a 20,000-request two-thread engine stress test. Recorded checks are dated in the project history; tests run through the commands above or GitHub Actions, rather than automatically on application startup.

### Benchmark scope

```bash
./build/release/fastbid_bench
```

The benchmark warms up with 10,000 requests, then measures 100,000 alternating resting/full-fill requests across the configured products at production capacities. It includes inbound queue insertion, core processing, and outbound event draining. A C++ allocation guard detects calls to standard allocation operators in that measured workload. It is not a universal detector for every possible allocator or workload.

It excludes sockets, gateway parsing, snapshot work, browser activity, persistence, and realistic multi-client network contention. Previously observed WSL results were sub-microsecond for this workload; they are not an end-to-end latency guarantee. Record hardware, compiler, build flags, load, and workload whenever comparing results.

Dashboard latency is a different measurement: engine request processing through completion, including report-pressure pauses and scheduling interruptions. It excludes inbound queue wait and network ingress. p50 is the median; p99 describes the slower edge of the retained sample distribution. Sample drops are counted and can bias those values. Throughput includes processed NEW, CANCEL, and rejected requests, not just trades. An idle sample window has no meaningful percentile.

## Operations and troubleshooting

`http://localhost:8080/healthz` returns 200 when the bridge has received a snapshot for every configured base product and registered basket within two seconds, otherwise 503. It checks feed freshness, not every possible trading operation. The dashboard separately waits for its own engine connection and marks missing market data stale.

| Symptom | What to check |
| --- | --- |
| Docker command fails | Docker daemon/Desktop is running, using Linux containers, and accessible to your user |
| Port 8080 already allocated | Stop the conflicting bridge/stack; inspect `docker ps` before starting another stack |
| Startup timeout | Inspect `docker compose -f compose.paper.yml logs --tail 100`; first-time builds need network access |
| Browser cannot connect | Use the exact local URL and check container status, HTTP health, and allowed origin |
| Dashboard shows stale data | Inspect engine and UDP-feed logs; the UI does not generate substitute data |
| An order does not trade | The opposite book may be empty or prices may not cross |
| Price or quantity rejected | Use UI dollars in [5,15], at most four decimals, and a positive integer quantity |
| Open orders disappear after refresh | New browser session; older orders can remain in the engine until matched or stack reset |
| Connection closes under load | Review malformed input, rate limits, and slow-consumer buffer limits; do not blindly retry |
| Changes are not reflected | Rebuild with `paper.ps1 start` or Compose `up --build`; reset alone reuses images |
| Native frontend fails to build | Use the documented Node version and `npm ci` from `client/` |

On SIGINT/SIGTERM [process stop signals], IO stops admitting requests. The engine drains requests already in its inbound queue. After engine completion, application TCP buffers get up to two seconds to flush. Partial or buffered requests not yet admitted are not guaranteed execution. Compose allows 30 seconds before forced termination, so very large drains can exceed that outer limit. Resting orders are discarded at exit.

## Limitations and future work

The current scope is local paper trading and systems engineering. Before live trading, the project needs durable journal/replay [saved operations that can rebuild state], authenticated accounts, account risk/credit checks, duplicate-request suppression, reconnect and execution recovery, failover, authenticated market data, and target-hardware capacity testing.

There is no automatic cancel-on-disconnect, user account ledger, market order, historical trade replay, or guaranteed delivery after disconnection. Sequence and exchange IDs restart with the engine process. Origin filtering and session ownership are not account authentication. Keep the local stack private.

Performance work should preserve correctness first: benchmark representative sparse/dense books, large sweeps, cancellations, many clients, saturated queues, and shutdown behavior before making latency claims.
