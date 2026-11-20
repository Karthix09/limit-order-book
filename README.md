# Limit Order Book Simulator

A real-time limit order book (LOB) with a C++20 matching engine, a built-in market-maker bot and a browser-based trading UI. The engine streams over WebSockets.

The backend has **no third-party dependencies**. Its HTTP/1.1 + WebSocket (RFC 6455) server is written directly on POSIX sockets, so CMake has nothing to download and the project builds offline on any Linux or macOS machine with a C++20 compiler.

## Features

**Matching engine**
- Strict price-time priority (FIFO within each price level). Trades execute at the resting order's price.
- Order types: **Limit (GTC)**, **Market**, **Immediate-or-Cancel**, plus **cancel by order ID**.
- Bids are kept in a `std::map<Price, Level, std::greater<>>` and asks in a `std::map<Price, Level, std::less<>>`, so `begin()` is always the best price on each side.
- Each level holds a FIFO `std::list<Order>`. An `std::unordered_map<OrderId, Locator>` stores the level iterator and list iterator for every resting order, which makes lookup and cancellation **O(1)** (an emptied level is erased by iterator).
- Prices are integer ticks (tick size 0.01), so there is no floating-point drift inside the book.

**Market-maker bot** (background thread)
- A latent fair value follows a Gaussian random walk, with weak reversion toward the live mid so manual sweeps get absorbed.
- Order flow mix: about 55% passive limit orders around fair value, 33% cancels (half of them pull the quote furthest from fair), 12% aggressive market or IOC orders that lean toward fair value.
- Seeds 15 levels per side at startup and refills a side if it is ever swept empty.
- Can be paused/resumed and its rate changed (1–2000 actions/s) live from the UI or the API.

**Frontend** (`web/index.html`, a single file with no build step)
- Order book ladder: green bids, red asks, cumulative depth bars, and the spread row with last price, spread, bps and mid. Click a level to copy its price into the order form.
- Order entry for Buy/Sell Limit, Market and IOC orders, with Best Bid / Mid / Best Ask shortcuts and a notional estimate.
- Recent trades tape (price, size, time with milliseconds, aggressor side) with flash highlighting.
- Live price chart (last trade plus bid/ask band), your open orders with cancel buttons, and an activity log.
- Resting orders you own are marked on the ladder, and passive fills are pushed to you in real time.
- Auto-reconnects with backoff. The server cancels your resting orders when you disconnect (cancel-on-disconnect).

## Directory structure

```
OrderBook/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── app/Gateway.hpp          # engine + bot <-> network glue, JSON protocol
│   ├── lob/Types.hpp            # Order, Trade, ExecutionReport, enums
│   ├── lob/OrderBook.hpp        # single-threaded price-time priority book
│   ├── lob/MatchingEngine.hpp   # thread-safe facade, trade buffers, stats
│   ├── lob/MarketMaker.hpp      # random-walk order-flow bot
│   ├── net/Crypto.hpp           # SHA-1 + Base64 for the WebSocket handshake
│   ├── net/WebServer.hpp        # HTTP + WebSocket server (POSIX sockets)
│   └── util/Json.hpp            # flat JSON parser / string quoting
├── src/
│   ├── main.cpp
│   ├── OrderBook.cpp
│   ├── MatchingEngine.cpp
│   ├── MarketMaker.cpp
│   ├── app/Gateway.cpp
│   ├── net/Crypto.cpp
│   ├── net/WebServer.cpp
│   └── util/Json.cpp
├── tests/test_main.cpp          # unit tests (matching, cancel, IOC, JSON, handshake)
└── web/index.html               # trading UI
```

## Build and run

Requirements: CMake ≥ 3.16 and a C++20 compiler (GCC ≥ 10 or Clang ≥ 12 / Apple Clang ≥ 13).

```bash
# macOS:          xcode-select --install && brew install cmake
# Debian/Ubuntu:  sudo apt install build-essential cmake

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure     # optional: run unit tests
./build/lob_server
```

Open **http://localhost:8080** in a browser. Stop the server with `Ctrl+C`.

Without CMake, a direct compiler invocation also works:

```bash
mkdir -p build
c++ -std=c++20 -O2 -pthread -Iinclude -DLOB_WEB_ROOT="\"$PWD/web\"" \
    src/*.cpp src/app/*.cpp src/net/*.cpp src/util/*.cpp -o build/lob_server
./build/lob_server
```

### Server options

| Option | Default | Description |
| --- | --- | --- |
| `--port N` | `8080` | HTTP/WebSocket port |
| `--bind ADDR` | `127.0.0.1` | Bind address (`0.0.0.0` to expose on the LAN) |
| `--web DIR` | `<source>/web` | Directory containing `index.html` |
| `--rate N` | `40` | Bot actions per second (1–2000) |
| `--mid PRICE` | `100.00` | Initial mid price |
| `--depth N` | `25` | Price levels per side streamed to clients |
| `--publish-ms N` | `100` | Snapshot publish interval |
| `--paused` | off | Start with the bot paused |
| `--seed N` | random | RNG seed for reproducible bot flow |

## Protocol

### WebSocket (`ws://host:port/ws`)

Client → server (all fields flat JSON; `reqId` is echoed back in the ack):

```json
{"action":"order","reqId":1,"side":"buy","type":"limit","price":100.25,"qty":10}
{"action":"order","reqId":2,"side":"sell","type":"market","qty":5}
{"action":"order","reqId":3,"side":"buy","type":"ioc","price":100.30,"qty":20}
{"action":"cancel","reqId":4,"orderId":123}
{"action":"bot","reqId":5,"running":false}
{"action":"bot","reqId":6,"rate":200}
```

Server → client:

| `type` | When | Contents |
| --- | --- | --- |
| `hello` | on connect | `clientId`, `tickSize`, bot state, current book, last 100 trades |
| `update` | every `--publish-ms` | `book` (`[price, qty, orderCount]` per level), new `trades`, `stats`, `bot` |
| `ack` | reply to a command | `ok`, `status` (`resting`, `partially_filled`, `filled`, `cancelled`, `rejected`), fills, `avgPrice`, `reason` |
| `fill` | your resting order traded | `orderId`, `price`, `qty`, `remaining` |

WebSocket clients can cancel only their own orders. Their resting orders are cancelled when they disconnect.

### HTTP

| Method & path | Description |
| --- | --- |
| `GET /api/health` | Liveness and connected client count |
| `GET /api/book?depth=N` | Book snapshot and stats |
| `GET /api/trades?limit=N` | Recent trades (oldest first) |
| `POST /api/order` | Same body as the WS `order` command |
| `POST /api/cancel` | `{"orderId":123}`. REST acts as an admin and can cancel any order |
| `GET /api/bot`, `POST /api/bot` | Read or change bot state: `{"running":true,"rate":100}` |

```bash
curl -s localhost:8080/api/book?depth=5
curl -s -X POST localhost:8080/api/order -d '{"side":"buy","type":"limit","price":99.50,"qty":25}'
curl -s -X POST localhost:8080/api/bot   -d '{"running":false}'
```

## Design notes

- **Threads.** There is one acceptor thread, one thread per connection, the bot thread and a publisher thread. Every book access goes through a single mutex in `MatchingEngine`, which keeps matching deterministic. Commands are handled synchronously on the connection's thread, so a client always gets its ack before any later updates for that order.
- **Publishing.** The book is snapshotted at a fixed rate instead of per event, so UI bandwidth stays bounded no matter how fast the bot runs. Trades are buffered between snapshots so none are skipped, and passive fills are routed privately to the order's owner.
- **Back-pressure.** Sockets use a 2 s send timeout. A client that can't keep up is disconnected so it can't stall the publisher for everyone else.
- **Scope.** This is a simulator for one instrument. It has no authentication or persistence, and the server binds to localhost by default.
