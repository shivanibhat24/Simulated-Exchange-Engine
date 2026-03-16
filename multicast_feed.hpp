#pragma once
#include <boost/asio.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include "types.hpp"
#include "spsc_queue.hpp"

namespace asio = boost::asio;
using udp      = asio::ip::udp;

static constexpr size_t  RETRANS_BUF_SIZE = 4096; // must be power of two
static constexpr size_t  FEED_QUEUE_CAP   = 4096;

// ─── Wire packet (binary, fixed-size, no padding) ─────────────────────────────
#pragma pack(push, 1)
enum class MsgTag : uint8_t { ExecReport = 1, DepthUpdate = 2 };

struct FeedPacket {
    SeqNum  seq      = 0;
    MsgTag  tag      = MsgTag::ExecReport;
    uint8_t _pad[7]  = {};
    union {
        ExecutionReport exec;
        DepthUpdate     depth;
    } body;
};
#pragma pack(pop)
static_assert(sizeof(FeedPacket) < 1400, "Packet must fit in one UDP datagram");

// ─── Multicast feed publisher ──────────────────────────────────────────────────
// Reads ExecutionReports and DepthUpdates from SPSCQueues and publishes
// them over UDP multicast with monotonic sequence numbers.
// A ring buffer of the last RETRANS_BUF_SIZE packets enables gap-fill.
class MulticastFeed {
    asio::io_context&                       ioc_;
    udp::socket                             socket_;
    udp::endpoint                           mcast_ep_;

    // Retransmission buffer — ring buffer, index = seq % RETRANS_BUF_SIZE
    std::array<FeedPacket, RETRANS_BUF_SIZE> retrans_buf_{};

    SPSCQueue<ExecutionReport, FEED_QUEUE_CAP>& exec_in_;
    SPSCQueue<DepthUpdate,     FEED_QUEUE_CAP>& depth_in_;

    // TCP repair channel for retransmit requests
    asio::ip::tcp::acceptor repair_acceptor_;

    std::atomic<bool>&  running_;
    SeqNum              seq_ = 0;

    void publish(FeedPacket& pkt) {
        pkt.seq = ++seq_;
        // Store in retransmission ring buffer
        retrans_buf_[seq_ & (RETRANS_BUF_SIZE - 1)] = pkt;
        // Fire-and-forget async send — no completion handler needed
        socket_.async_send_to(
            asio::buffer(&pkt, sizeof(pkt)), mcast_ep_,
            [](boost::system::error_code ec, size_t) {
                if (ec) std::cerr << "[feed] send error: " << ec.message() << "\n";
            });
    }

    void drain_queues() {
        // Drain exec reports
        while (auto er = exec_in_.pop()) {
            FeedPacket pkt{};
            pkt.tag      = MsgTag::ExecReport;
            pkt.body.exec = *er;
            publish(pkt);
        }
        // Drain depth updates
        while (auto du = depth_in_.pop()) {
            FeedPacket pkt{};
            pkt.tag        = MsgTag::DepthUpdate;
            pkt.body.depth = *du;
            publish(pkt);
        }
    }

    // ─── Repair channel (TCP, handles NACK retransmit requests) ───────────────
    // Clients send: "NACK <from_seq> <to_seq>\n"
    // Server sends back: raw FeedPacket bytes for that range
    void start_repair_channel() {
        repair_acceptor_.async_accept(
            [this](boost::system::error_code ec,
                   asio::ip::tcp::socket sock) {
                if (!ec) handle_repair(std::move(sock));
                start_repair_channel();
            });
    }

    void handle_repair(asio::ip::tcp::socket sock) {
        auto sp = std::make_shared<asio::ip::tcp::socket>(std::move(sock));
        auto buf = std::make_shared<std::array<char, 64>>();
        sp->async_read_some(
            asio::buffer(*buf),
            [this, sp, buf](boost::system::error_code ec, size_t n) {
                if (ec || n == 0) return;
                SeqNum from = 0, to = 0;
                sscanf(buf->data(), "NACK %llu %llu",
                    (unsigned long long*)&from,
                    (unsigned long long*)&to);
                std::cout << "[feed] retransmit request: " << from << "-" << to << "\n";

                // Validate range is within our retransmit buffer
                SeqNum oldest = (seq_ > RETRANS_BUF_SIZE)
                                ? seq_ - RETRANS_BUF_SIZE + 1 : 1;
                from = std::max(from, oldest);
                to   = std::min(to,   seq_);

                // Build retransmit response
                auto resp = std::make_shared<std::vector<char>>();
                for (SeqNum s = from; s <= to; ++s) {
                    const FeedPacket& pkt = retrans_buf_[s & (RETRANS_BUF_SIZE - 1)];
                    if (pkt.seq != s) continue; // evicted from buffer
                    size_t off = resp->size();
                    resp->resize(off + sizeof(FeedPacket));
                    memcpy(resp->data() + off, &pkt, sizeof(FeedPacket));
                }
                asio::async_write(*sp, asio::buffer(*resp),
                    [sp, resp](boost::system::error_code, size_t) {});
            });
    }

public:
    MulticastFeed(asio::io_context&                       ioc,
                  const std::string&                      mcast_addr,
                  uint16_t                                mcast_port,
                  uint16_t                                repair_port,
                  SPSCQueue<ExecutionReport, FEED_QUEUE_CAP>& exec_in,
                  SPSCQueue<DepthUpdate,     FEED_QUEUE_CAP>& depth_in,
                  std::atomic<bool>&                      running)
        : ioc_(ioc),
          socket_(ioc, udp::v4()),
          mcast_ep_(asio::ip::make_address(mcast_addr), mcast_port),
          exec_in_(exec_in),
          depth_in_(depth_in),
          repair_acceptor_(ioc,
              asio::ip::tcp::endpoint(asio::ip::tcp::v4(), repair_port)),
          running_(running)
    {
        // Allow multiple processes to bind the same multicast port
        socket_.set_option(udp::socket::reuse_address(true));
        // Set TTL so packets don't leave the local subnet
        socket_.set_option(asio::ip::multicast::hops(1));
        std::cout << "[feed] multicast " << mcast_addr << ":" << mcast_port
                  << "  repair TCP port " << repair_port << "\n";
        start_repair_channel();
    }

    // Called from the feed publisher thread — drains both queues and
    // posts async sends onto the io_context.
    void poll() { drain_queues(); }
};
