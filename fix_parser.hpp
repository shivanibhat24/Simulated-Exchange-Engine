#pragma once
#include <string_view>
#include <charconv>
#include <cstring>
#include "types.hpp"

// ─── FIX 4.2 wire format helpers ─────────────────────────────────────────────
// FIX messages: "8=FIX.4.2\x01" "9=<len>\x01" ... "<tag>=<val>\x01" ...
// SOH delimiter = '\x01'
// Zero-copy: all parsing operates on string_view into the recv buffer.

static constexpr char SOH = '\x01';

inline bool parse_int(std::string_view sv, int64_t& out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{};
}

inline bool parse_uint(std::string_view sv, uint64_t& out) {
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc{};
}

// Returns parsed OrderRequest, or nullopt if message is not a NewOrderSingle
// or OrderCancelRequest.
inline std::optional<OrderRequest> parse_fix(std::string_view msg) {
    OrderRequest req{};
    int msg_type_tag = 0; // 35=

    auto next_field = [&](size_t& pos) -> std::pair<int, std::string_view> {
        if (pos >= msg.size()) return {-1, {}};
        size_t eq = msg.find('=', pos);
        if (eq == std::string_view::npos) return {-1, {}};
        int tag = 0;
        std::from_chars(msg.data() + pos, msg.data() + eq, tag);
        size_t soh = msg.find(SOH, eq + 1);
        if (soh == std::string_view::npos) soh = msg.size();
        std::string_view val = msg.substr(eq + 1, soh - eq - 1);
        pos = soh + 1;
        return {tag, val};
    };

    size_t pos = 0;
    bool is_order = false;
    bool is_cancel = false;

    while (pos < msg.size()) {
        auto [tag, val] = next_field(pos);
        if (tag < 0) break;

        int64_t  ival = 0;
        uint64_t uval = 0;

        switch (tag) {
            case 35: // MsgType
                if (val == "D")  { is_order  = true; req.type = RequestType::NewOrder;    }
                if (val == "F")  { is_cancel = true; req.type = RequestType::CancelOrder; }
                break;
            case 11: // ClOrdID → order_id (parse as number)
                parse_uint(val, uval); req.order_id = uval; break;
            case 49: // SenderCompID → client_id
                parse_uint(val, uval); req.client_id = static_cast<ClientId>(uval); break;
            case 55: // Symbol
                std::strncpy(req.symbol, val.data(), std::min(val.size(), size_t(8))); break;
            case 54: // Side: 1=buy, 2=sell
                req.side = (val == "1") ? Side::Buy : Side::Sell; break;
            case 38: // OrderQty
                parse_int(val, ival); req.qty = ival; break;
            case 44: // Price (in ticks × 100, e.g. "10050" = 100.50)
                parse_int(val, ival); req.price = ival; break;
            case 40: // OrdType: 1=market, 2=limit
                req.ord_type = (val == "1") ? OrdType::Market : OrdType::Limit; break;
            default: break;
        }
    }

    if (!is_order && !is_cancel) return std::nullopt;
    return req;
}

// ─── FIX message builder ──────────────────────────────────────────────────────
// Writes into a caller-supplied buffer. Returns bytes written.
// All writes go through a single pointer — no heap allocation.

class FixBuilder {
    char*  buf_;
    size_t cap_;
    size_t pos_ = 0;

    void append(std::string_view sv) {
        if (pos_ + sv.size() < cap_) {
            memcpy(buf_ + pos_, sv.data(), sv.size());
            pos_ += sv.size();
        }
    }
    void append_char(char c) { if (pos_ < cap_) buf_[pos_++] = c; }

    void field(int tag, std::string_view val) {
        char tmp[16];
        auto [p, _] = std::to_chars(tmp, tmp + 16, tag);
        append({tmp, static_cast<size_t>(p - tmp)});
        append_char('=');
        append(val);
        append_char(SOH);
    }

    void field(int tag, int64_t val) {
        char tmp[24];
        auto [p, _] = std::to_chars(tmp, tmp + 24, val);
        field(tag, {tmp, static_cast<size_t>(p - tmp)});
    }

public:
    FixBuilder(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    // Builds an ExecutionReport (tag 35=8)
    size_t build_exec_report(const ExecutionReport& er) {
        pos_ = 0;
        // Header
        append("8=FIX.4.2"); append_char(SOH);
        field(35, "8");

        // Body
        field(11, static_cast<int64_t>(er.order_id));
        field(14, er.exec_qty);                   // CumQty
        field(17, static_cast<int64_t>(er.seq));   // ExecID
        field(20, "0");                            // ExecTransType = New
        field(31, er.exec_price);                  // LastPx
        field(32, er.exec_qty);                    // LastShares
        field(37, static_cast<int64_t>(er.order_id)); // OrderID
        field(38, er.exec_qty + er.leaves_qty);    // OrderQty
        field(39, static_cast<int64_t>(          // OrdStatus
            static_cast<int>(er.status)));
        field(54, er.side == Side::Buy ? "1" : "2");
        field(55, {er.symbol, strnlen(er.symbol, 8)});
        field(151, er.leaves_qty);                 // LeavesQty

        // Checksum (sum of all bytes mod 256, 3-digit zero-padded)
        int sum = 0;
        for (size_t i = 0; i < pos_; ++i) sum += (unsigned char)buf_[i];
        sum &= 0xFF;
        char cs[4] = {'0' + (char)(sum/100), '0' + (char)((sum/10)%10), '0' + (char)(sum%10), 0};
        field(10, {cs, 3});

        return pos_;
    }
};
