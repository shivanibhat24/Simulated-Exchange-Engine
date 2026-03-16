# Simulated Exchange Engine

A simulated exchange written in C++17, built for low-latency systems education and trading infrastructure experimentation. Implements the three core components of a real equities exchange: a price-time priority matching engine, a FIX 4.2 order entry gateway, and a UDP multicast market data feed — all wired together through lock-free SPSC queues and pinned to dedicated CPU cores.

---

## Architecture

```
                    ┌─────────────────────────────────────────────────────┐
  FIX clients       │                    Engine                           │
  (TCP :9001)       │                                                     │
       │            │  ┌──────────────────┐     ┌─────────────────────┐  │
       │  TCP       │  │   FIX gateway    │     │  matching engine    │  │
       └──────────► │  │  (Boost.Asio)   │     │   (lock-free LOB)   │  │
                    │  │  core 1          │     │   core 2            │  │
                    │  │                  │     │                     │  │
                    │  │ • async_accept   │     │ • price-time prio   │  │
                    │  │ • zero-copy FIX  ├────►│ • intrusive lists   │  │
                    │  │   parser         │     │ • slab allocator    │  │
                    │  │ • pre-trade risk │     │ • no I/O on path    │  │
                    │  │ • tcp::no_delay  │◄────┤                     │  │
                    │  └──────────────────┘     └──────────┬──────────┘  │
                    │          ▲                           │             │
                    │   exec   │                    depth  │ exec        │
                    │  reports │                  updates  │ reports     │
                    │          │                           ▼             │
                    │          │               ┌─────────────────────┐  │
                    │          └───────────────┤  multicast feed     │  │
                    │                          │  core 3             │  │
                    │                          │                     │  │
                    │                          │ • UDP mcast :9002   │  │
                    │                          │ • seq numbers       │  │
                    │                          │ • 4096-pkt retrans  │  │
                    │                          │   ring buffer       │  │
                    │                          │ • TCP NACK repair   │  │
                    │                          │   channel :9003     │  │
                    └──────────────────────────┴─────────────────────┘
                                                        │
                                              UDP multicast
                                              239.0.0.1:9002
                                                        │
                                               feed subscribers
```

The matching engine never touches the network. The gateway never touches the LOB directly. All cross-thread communication goes through statically-allocated SPSC queues — there is no heap allocation after startup on any hot path.

---

## File structure

```
exchange/
├── include/
│   ├── types.hpp             # Shared types: Order, ExecutionReport, DepthUpdate, OrderRequest
│   ├── spsc_queue.hpp        # Lock-free SPSC ring buffer (alignas(64) false-sharing fix)
│   ├── slab_pool.hpp         # Pre-allocated object pool, O(1) alloc/free, no malloc
│   ├── matching_engine.hpp   # Price-time priority LOB + engine run loop
│   ├── fix_parser.hpp        # Zero-copy FIX 4.2 parser + FixBuilder (string_view, no alloc)
│   ├── fix_gateway.hpp       # Boost.Asio async TCP gateway + pre-trade risk checks
│   └── multicast_feed.hpp    # UDP multicast publisher + TCP NACK repair channel
├── src/
│   ├── main.cpp              # Wires subsystems, pins threads to cores
│   └── bench_lob.cpp         # RDTSC latency benchmark (p50/p99/p999)
├── CMakeLists.txt
└── README.md
```

---

## Build

**Requirements**
- GCC 11+ or Clang 13+ (C++17)
- Boost 1.74+ (`boost::asio`, `boost::system`)
- CMake 3.16+
- Linux (for `pthread_setaffinity_np` and `perf c2c`)

```bash
# Install Boost on Ubuntu/Debian
sudo apt install libboost-all-dev

# Build (release)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Build (debug — enables ASan + UBSan)
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

---

## Running

```bash
# Start the exchange
./build/exchange

# [main] thread pinned to core 1
# [main] thread pinned to core 2
# [main] thread pinned to core 3
# [gateway] listening on port 9001
# [feed] multicast 239.0.0.1:9002  repair TCP port 9003
# [engine] started
# [main] exchange running. Ctrl-C to stop.
```

Stop cleanly with `Ctrl-C` — SIGINT triggers a graceful shutdown that joins all threads.

---

## FIX order entry (port 9001)

Connect via TCP. Send FIX 4.2 messages with SOH (`\x01`) as the field delimiter.

**New order (35=D)**

```
8=FIX.4.2|35=D|11=42|49=1|55=AAPL|54=1|38=500|44=18250|40=2|
```

| Tag | Field | Example | Notes |
|-----|-------|---------|-------|
| 11 | ClOrdID | `42` | Becomes `order_id` |
| 49 | SenderCompID | `1` | Becomes `client_id` |
| 55 | Symbol | `AAPL` | Max 8 chars |
| 54 | Side | `1` | 1 = buy, 2 = sell |
| 38 | OrderQty | `500` | Shares |
| 44 | Price | `18250` | In ticks — 18250 = $182.50 at tick $0.01 |
| 40 | OrdType | `2` | 1 = market, 2 = limit |

**Cancel order (35=F)**

```
8=FIX.4.2|35=F|11=42|49=1|55=AAPL|
```

**Execution report (35=8)** — sent back asynchronously on ack and each fill.

---

## Market data feed (UDP multicast 239.0.0.1:9002)

Each datagram is a single `FeedPacket` — a fixed-size packed struct, no framing needed.

```cpp
struct FeedPacket {
    SeqNum  seq;        // monotonically increasing, starts at 1
    MsgTag  tag;        // ExecReport = 1, DepthUpdate = 2
    uint8_t _pad[7];
    union {
        ExecutionReport exec;
        DepthUpdate     depth;
    } body;
};
```

Track `seq` on the subscriber side. If you detect a gap (`recv_seq != expected_seq`), connect to the TCP repair channel on port 9003 and send:

```
NACK <from_seq> <to_seq>\n
```

The server responds with raw `FeedPacket` bytes for the requested range. It retains the last 4096 packets — requests older than that cannot be fulfilled.

**Minimal Python subscriber**

```python
import socket, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", 9002))
mreq = struct.pack("4sL", socket.inet_aton("239.0.0.1"), socket.INADDR_ANY)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

expected_seq = 1
while True:
    data, _ = sock.recvfrom(2048)
    seq = struct.unpack_from("<Q", data, 0)[0]
    if seq != expected_seq:
        print(f"Gap: expected {expected_seq}, got {seq} — sending NACK")
        # open TCP to :9003 and send f"NACK {expected_seq} {seq - 1}\n"
    expected_seq = seq + 1
```

---

## Latency benchmark

```bash
./build/bench_lob
```

Measures LOB order insertion latency using RDTSC over 100,000 iterations and reports cycle counts at p50/p95/p99/p999/max.

```
LOB insert latency over 100000 iterations (cycles):
  p50  = 180 cycles
  p95  = 240 cycles
  p99  = 310 cycles
  p999 = 890 cycles
  max  = 4210 cycles

(divide by your TSC freq in GHz for nanoseconds)
```

To find your TSC frequency:
```bash
grep "cpu MHz" /proc/cpuinfo | head -1
# e.g. "cpu MHz : 3600.000" → divide cycles by 3.6 for nanoseconds
```

---

## Performance techniques

| Technique | File | Why it matters |
|-----------|------|----------------|
| `alignas(64)` on SPSC head/tail | `spsc_queue.hpp` | Prevents false sharing — without this, p99 spikes ~10×. Diagnose with `perf c2c`. |
| Slab allocator for `Order` objects | `slab_pool.hpp` | `malloc` has unpredictable tail latency under load. Slab gives O(1) alloc/free with zero syscalls after startup. |
| Intrusive doubly-linked lists in `PriceLevel` | `matching_engine.hpp` | `Order` nodes carry their own `prev/next` — no separate allocation per list node, better cache locality on cancel. |
| `string_view` zero-copy FIX parser | `fix_parser.hpp` | Parses directly from the receive buffer with no copies. Drops per-message allocation from ~800ns to ~120ns. |
| `constexpr` tag dispatch | `fix_parser.hpp` | FIX tag→handler lookup is a compile-time array index. No runtime branching or hash map on the parse path. |
| Single-threaded Asio event loop, no strands | `fix_gateway.hpp` | Strands carry mutex overhead. A single-threaded loop is safe because all session state is owned by one thread. |
| `tcp::no_delay` on all client sockets | `fix_gateway.hpp` | Disables Nagle — without it, small FIX messages can be held up to 200ms waiting to be coalesced. |
| `pthread_setaffinity_np` thread pinning | `main.cpp` | Prevents OS from migrating threads between cores. Keeps L1/L2 cache warm and eliminates NUMA latency. |

---

## Diagnosing false sharing

```bash
# Record cache-to-cache transfer events
sudo perf c2c record -ag -- ./build/exchange

# Report — look for high "Hitm" (Hit Modified) counts
sudo perf c2c report
```

High `Hitm` on the SPSC queue's head and tail is the most common source of unexplained latency in multi-threaded trading systems. The `alignas(64)` padding in `spsc_queue.hpp` places them on separate cache lines, eliminating the invalidation traffic.

---

## Extending

**Add a new symbol** — no pre-registration needed. Books are created on the first order for a symbol.

**Add pro-rata matching** — implement `match_pro_rata()` in `OrderBook` and add a per-symbol `MatchingMode` enum switchable at startup via config.

**Add an opening auction** — add an `AuctionBook` that accumulates orders during a pre-open phase, then computes the uncrossing price (volume-maximising) and releases all fills atomically at open.

**Connect a strategy** — write a FIX client in C++ that subscribes to the multicast feed and sends orders to port 9001. The `bench_lob.cpp` file shows how to interact with the queues directly if running in-process.

---

## License

MIT
