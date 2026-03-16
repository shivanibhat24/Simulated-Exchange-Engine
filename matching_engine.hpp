#pragma once
#include <map>
#include <unordered_map>
#include <functional>
#include <atomic>
#include "types.hpp"
#include "slab_pool.hpp"
#include "spsc_queue.hpp"

// ─── PriceLevel: intrusive doubly-linked list of orders ──────────────────────
struct PriceLevel {
    Order* head  = nullptr;
    Order* tail  = nullptr;
    Qty    total = 0;

    void push_back(Order* o) {
        o->prev = tail; o->next = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        total += o->leaves_qty;
    }

    void remove(Order* o) {
        total -= o->leaves_qty;
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        o->prev = o->next = nullptr;
    }
};

// ─── OrderBook: one symbol, buy/sell sides ───────────────────────────────────
class OrderBook {
    // Bids: highest price first (reverse map)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    // Asks: lowest price first
    std::map<Price, PriceLevel, std::less<Price>>    asks_;

public:
    // Returns filled qty. Emits fills via callback.
    Qty match(Order* aggressor,
              std::function<void(Order*, Order*, Price, Qty)> on_fill) {

        auto& passive_side = (aggressor->side == Side::Buy) ? asks_ : bids_;
        Qty   filled       = 0;

        while (aggressor->leaves_qty > 0 && !passive_side.empty()) {
            auto it         = passive_side.begin();
            Price lvl_price = it->first;

            // Price check for limit orders
            if (aggressor->type == OrdType::Limit) {
                if (aggressor->side == Side::Buy  && aggressor->price < lvl_price) break;
                if (aggressor->side == Side::Sell && aggressor->price > lvl_price) break;
            }

            PriceLevel& lvl = it->second;
            while (lvl.head && aggressor->leaves_qty > 0) {
                Order* resting = lvl.head;
                Qty    qty     = std::min(aggressor->leaves_qty, resting->leaves_qty);

                on_fill(aggressor, resting, lvl_price, qty);

                aggressor->leaves_qty -= qty;
                resting->leaves_qty   -= qty;
                lvl.total             -= qty;
                filled                += qty;

                if (resting->leaves_qty == 0)
                    lvl.remove(resting);
            }
            if (lvl.head == nullptr) passive_side.erase(it);
        }
        return filled;
    }

    void add_resting(Order* o) {
        if (o->side == Side::Buy)
            bids_[o->price].push_back(o);
        else
            asks_[o->price].push_back(o);
    }

    bool cancel(OrderId id, Side side) {
        auto& side_map = (side == Side::Buy) ? bids_ : asks_;
        for (auto& [price, lvl] : side_map) {
            for (Order* o = lvl.head; o; o = o->next) {
                if (o->order_id == id) {
                    lvl.remove(o);
                    if (!lvl.head) side_map.erase(price);
                    return true;
                }
            }
        }
        return false;
    }

    // Best bid/ask for depth snapshots
    Price best_bid() const { return bids_.empty()  ? -1 : bids_.begin()->first;  }
    Price best_ask() const { return asks_.empty()   ? -1 : asks_.begin()->first;  }
    Qty   bid_qty()  const { return bids_.empty()   ?  0 : bids_.begin()->second.total; }
    Qty   ask_qty()  const { return asks_.empty()   ?  0 : asks_.begin()->second.total; }
};

// ─── MatchingEngine ───────────────────────────────────────────────────────────
class MatchingEngine {
    static constexpr size_t MAX_ORDERS    = 65536;
    static constexpr size_t QUEUE_CAP     = 4096;

    SlabPool<Order, MAX_ORDERS>          pool_;
    std::unordered_map<std::string, OrderBook> books_;
    std::unordered_map<OrderId, Order*>  live_orders_;

    // Inbound requests from FIX gateway (gateway is producer, engine is consumer)
    SPSCQueue<OrderRequest, QUEUE_CAP>&  inbound_;
    // Outbound fills to feed publisher (engine is producer)
    SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_out_;
    SPSCQueue<DepthUpdate,     QUEUE_CAP>& depth_out_;

    std::atomic<bool>& running_;
    SeqNum             seq_ = 0;

    void process(const OrderRequest& req) {
        if (req.type == RequestType::CancelOrder) {
            auto it = live_orders_.find(req.order_id);
            if (it == live_orders_.end()) return;
            Order* o = it->second;
            std::string sym(o->symbol, strnlen(o->symbol, 8));
            books_[sym].cancel(req.order_id, o->side);
            live_orders_.erase(it);
            pool_.free(o);

            ExecutionReport er{};
            er.order_id   = req.order_id;
            er.client_id  = req.client_id;
            er.status     = OrdStatus::Cancelled;
            er.seq        = ++seq_;
            memcpy(er.symbol, req.symbol, 8);
            while (!exec_out_.push(er)) { /* spin — should be rare */ }
            return;
        }

        // New order
        Order* o        = pool_.alloc();
        o->order_id     = req.order_id;
        o->client_id    = req.client_id;
        o->price        = req.price;
        o->qty          = req.qty;
        o->leaves_qty   = req.qty;
        o->side         = req.side;
        o->type         = req.ord_type;
        memcpy(o->symbol, req.symbol, 8);

        std::string sym(req.symbol, strnlen(req.symbol, 8));
        OrderBook& book = books_[sym];

        // Ack the new order
        ExecutionReport ack{};
        ack.order_id  = o->order_id;
        ack.client_id = o->client_id;
        ack.status    = OrdStatus::New;
        ack.leaves_qty = o->leaves_qty;
        ack.seq       = ++seq_;
        ack.side      = o->side;
        memcpy(ack.symbol, o->symbol, 8);
        while (!exec_out_.push(ack)) {}

        // Try to match
        book.match(o, [&](Order* agg, Order* rst, Price px, Qty qty) {
            auto emit = [&](Order* ord, OrdStatus st) {
                ExecutionReport er{};
                er.order_id   = ord->order_id;
                er.client_id  = ord->client_id;
                er.exec_price = px;
                er.exec_qty   = qty;
                er.leaves_qty = ord->leaves_qty;
                er.status     = st;
                er.side       = ord->side;
                er.seq        = ++seq_;
                memcpy(er.symbol, ord->symbol, 8);
                while (!exec_out_.push(er)) {}
            };

            OrdStatus agg_st = (agg->leaves_qty == 0) ? OrdStatus::Filled : OrdStatus::PartiallyFilled;
            OrdStatus rst_st = (rst->leaves_qty == 0) ? OrdStatus::Filled : OrdStatus::PartiallyFilled;
            emit(agg, agg_st);
            emit(rst, rst_st);

            // Emit depth update for the resting level that changed
            DepthUpdate du{};
            memcpy(du.symbol, rst->symbol, 8);
            du.price = px;
            du.qty   = rst->leaves_qty; // 0 = level gone
            du.side  = rst->side;
            du.seq   = seq_;
            while (!depth_out_.push(du)) {}

            if (rst->leaves_qty == 0) {
                live_orders_.erase(rst->order_id);
                pool_.free(rst);
            }
        });

        if (o->leaves_qty > 0) {
            // Resting in book
            book.add_resting(o);
            live_orders_[o->order_id] = o;

            // Emit depth update for new resting level
            DepthUpdate du{};
            memcpy(du.symbol, o->symbol, 8);
            du.price = o->price;
            du.qty   = o->leaves_qty;
            du.side  = o->side;
            du.seq   = seq_;
            while (!depth_out_.push(du)) {}
        } else {
            pool_.free(o);
        }
    }

public:
    MatchingEngine(SPSCQueue<OrderRequest,    QUEUE_CAP>& inbound,
                   SPSCQueue<ExecutionReport, QUEUE_CAP>& exec_out,
                   SPSCQueue<DepthUpdate,     QUEUE_CAP>& depth_out,
                   std::atomic<bool>&                     running)
        : inbound_(inbound), exec_out_(exec_out),
          depth_out_(depth_out), running_(running) {}

    // Run on a dedicated pinned thread — spins on inbound queue
    void run() {
        while (running_.load(std::memory_order_relaxed)) {
            if (auto req = inbound_.pop()) {
                process(*req);
            }
        }
    }
};
