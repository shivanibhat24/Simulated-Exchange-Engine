#include <boost/asio.hpp>
#include <thread>
#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>

#include "types.hpp"
#include "spsc_queue.hpp"
#include "matching_engine.hpp"
#include "fix_gateway.hpp"
#include "multicast_feed.hpp"

namespace asio = boost::asio;

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\n[main] shutting down...\n";
    g_running.store(false, std::memory_order_relaxed);
}

// Pin a thread to a specific CPU core using pthreads.
// In production: also set SCHED_FIFO priority and isolate with cpuset.
void pin_thread(std::thread& t, int core) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    int rc = pthread_setaffinity_np(t.native_handle(), sizeof(cs), &cs);
    if (rc != 0)
        std::cerr << "[main] WARNING: could not pin thread to core " << core << "\n";
    else
        std::cout << "[main] thread pinned to core " << core << "\n";
}

int main() {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Shared lock-free queues (all statically sized, no heap after init) ─────
    //
    //  Gateway  ──[inbound]──►  Matching engine
    //  Gateway  ◄─[exec_gw]──  Matching engine
    //  Feed     ◄─[exec_fd]──  Matching engine
    //  Feed     ◄─[depth]───   Matching engine

    static SPSCQueue<OrderRequest,    4096> q_inbound;
    static SPSCQueue<ExecutionReport, 4096> q_exec_gw;   // fills → gateway
    static SPSCQueue<ExecutionReport, 4096> q_exec_feed; // fills → feed
    static SPSCQueue<DepthUpdate,     4096> q_depth;     // depth → feed

    // ── Matching engine ────────────────────────────────────────────────────────
    MatchingEngine engine(q_inbound, q_exec_gw, q_depth, g_running);

    std::thread engine_thread([&] {
        std::cout << "[engine] started\n";
        engine.run();
        std::cout << "[engine] stopped\n";
    });
    pin_thread(engine_thread, 2); // dedicate core 2 to the engine

    // ── FIX gateway (runs on Boost.Asio io_context, single-threaded) ──────────
    asio::io_context gw_ioc;
    FIXGateway gateway(gw_ioc, 9001, q_inbound, q_exec_gw);

    std::thread gateway_thread([&] {
        std::cout << "[gateway] io_context running on core 1\n";
        // run() drives all async_accept / async_read_some / async_write ops
        gw_ioc.run();
        std::cout << "[gateway] io_context stopped\n";
    });
    pin_thread(gateway_thread, 1);

    // ── Multicast feed (runs on its own io_context) ────────────────────────────
    asio::io_context feed_ioc;
    MulticastFeed feed(
        feed_ioc,
        "239.0.0.1",  // multicast group — stays on local subnet (TTL=1)
        9002,         // multicast data port
        9003,         // TCP repair channel port
        q_exec_feed,
        q_depth,
        g_running);

    std::thread feed_thread([&] {
        std::cout << "[feed] publisher running on core 3\n";
        // Interleave queue draining with asio event loop:
        // poll() drains queues and posts async_send_to onto ioc;
        // poll_one() drives the io_context completion handlers.
        while (g_running.load(std::memory_order_relaxed)) {
            feed.poll();          // drain SPSCQueues → post async sends
            feed_ioc.poll_one();  // drive one completion handler
        }
        feed_ioc.stop();
        std::cout << "[feed] stopped\n";
    });
    pin_thread(feed_thread, 3);

    // ── Shutdown ───────────────────────────────────────────────────────────────
    std::cout << "[main] exchange running. Ctrl-C to stop.\n";

    // Wait until SIGINT sets g_running = false
    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop io_contexts (unblocks run())
    gw_ioc.stop();
    feed_ioc.stop();

    engine_thread.join();
    gateway_thread.join();
    feed_thread.join();

    std::cout << "[main] clean shutdown complete.\n";
    return 0;
}
