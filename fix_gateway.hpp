#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <iostream>
#include "types.hpp"
#include "fix_parser.hpp"
#include "spsc_queue.hpp"

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

static constexpr size_t QUEUE_CAP = 4096;

// ─── Pre-trade risk check (branch-free, no heap) ──────────────────────────────
struct RiskLimits {
    Qty   max_order_qty     = 10'000;
    Price max_order_notional = 100'000'00; // in ticks (= $1,000,000 at tick 0.01)
    bool  kill_switch       = false;
};

inline bool risk_check(const OrderRequest& req, const RiskLimits& lim) {
    if (lim.kill_switch)                     return false;
    if (req.qty   > lim.max_order_qty)       return false;
    if (req.price * req.qty > lim.max_order_notional) return false;
    return true;
}

// ─── FIX session (one per connected client) ───────────────────────────────────
class FIXSession : public std::enable_shared_from_this<FIXSession> {
    tcp::socket                           socket_;
    std::array<char, 4096>                recv_buf_{};
    char                                  send_buf_[512]{};
    SPSCQueue<OrderRequest, QUEUE_CAP>&   outbound_;   // → matching engine
    SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_in_;   // ← matching engine fills
    RiskLimits                            limits_;
    ClientId                              client_id_;
    std::atomic<uint64_t>                 next_order_id_{1};

public:
    FIXSession(tcp::socket                           sock,
               SPSCQueue<OrderRequest, QUEUE_CAP>&   outbound,
               SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_in,
               ClientId                              cid)
        : socket_(std::move(sock)),
          outbound_(outbound),
          exec_in_(exec_in),
          client_id_(cid)
    {}

    void start() { do_read(); }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(recv_buf_),
            [this, self](boost::system::error_code ec, size_t n) {
                if (ec) {
                    std::cerr << "[gateway] client " << client_id_
                              << " disconnected: " << ec.message() << "\n";
                    return;
                }
                handle_data({recv_buf_.data(), n});
                do_read();
            });
    }

    void handle_data(std::string_view data) {
        // In production: accumulate into a ring buffer and frame on checksum.
        // Here: treat each recv as a complete FIX message for simplicity.
        auto req_opt = parse_fix(data);
        if (!req_opt) return;

        OrderRequest& req = *req_opt;
        req.client_id     = client_id_;
        if (req.type == RequestType::NewOrder)
            req.order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);

        if (!risk_check(req, limits_)) {
            send_reject(req.order_id, "Risk limit breach");
            return;
        }

        // Enqueue to matching engine — non-blocking spin is safe here
        // because the engine drains faster than the gateway produces.
        while (!outbound_.push(req)) { /* spin */ }

        // Drain any pending execution reports back to this client
        // NOTE: In production, execution reports are routed per client_id.
        // Here we drain all reports from the shared queue opportunistically.
        drain_exec_reports();
    }

    void drain_exec_reports() {
        while (auto er = exec_in_.pop()) {
            if (er->client_id != client_id_) continue; // not ours
            FixBuilder builder(send_buf_, sizeof(send_buf_));
            size_t len = builder.build_exec_report(*er);
            // async_write ensures the full message is sent
            auto self = shared_from_this();
            asio::async_write(
                socket_,
                asio::buffer(send_buf_, len),
                [self](boost::system::error_code ec, size_t) {
                    if (ec) std::cerr << "[gateway] send error: " << ec.message() << "\n";
                });
        }
    }

    void send_reject(OrderId oid, std::string_view reason) {
        // Build a minimal OrderCancelReject (tag 35=9) inline
        int pos = snprintf(send_buf_, sizeof(send_buf_),
            "8=FIX.4.2\x01" "35=9\x01" "11=%llu\x01" "58=%.*s\x01" "10=000\x01",
            (unsigned long long)oid,
            (int)reason.size(), reason.data());
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(send_buf_, pos),
            [self](boost::system::error_code, size_t) {});
    }
};

// ─── FIX gateway: accepts connections, spawns sessions ────────────────────────
class FIXGateway {
    asio::io_context&                     ioc_;
    tcp::acceptor                         acceptor_;
    SPSCQueue<OrderRequest, QUEUE_CAP>&   outbound_;
    SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_in_;
    std::atomic<ClientId>                 next_client_{1};

public:
    FIXGateway(asio::io_context&                     ioc,
               uint16_t                              port,
               SPSCQueue<OrderRequest, QUEUE_CAP>&   outbound,
               SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_in)
        : ioc_(ioc),
          acceptor_(ioc, tcp::endpoint(tcp::v4(), port)),
          outbound_(outbound),
          exec_in_(exec_in)
    {
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        std::cout << "[gateway] listening on port " << port << "\n";
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket sock) {
                if (!ec) {
                    ClientId cid = next_client_.fetch_add(1, std::memory_order_relaxed);
                    std::cout << "[gateway] new client " << cid
                              << " from " << sock.remote_endpoint() << "\n";
                    // Disable Nagle — critical for low-latency
                    sock.set_option(tcp::no_delay(true));
                    std::make_shared<FIXSession>(
                        std::move(sock), outbound_, exec_in_, cid)->start();
                }
                do_accept();
            });
    }
};
