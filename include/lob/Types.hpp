#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using Price = std::int64_t;  // integer ticks; 1 tick = 1 / kTicksPerUnit
using Qty = std::int64_t;
using OwnerId = std::uint64_t;

inline constexpr Price kTicksPerUnit = 100;  // tick size 0.01
inline constexpr OwnerId kBotOwner = 0;
inline constexpr OwnerId kRestOwner = ~OwnerId{0};
inline constexpr Qty kMaxOrderQty = 1'000'000;
inline constexpr Price kMaxPrice = 1'000'000 * kTicksPerUnit;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market, IOC };
enum class OrderStatus : std::uint8_t { Resting, PartiallyFilled, Filled, Cancelled, Rejected };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view toString(Side s) noexcept { return s == Side::Buy ? "buy" : "sell"; }

constexpr std::string_view toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "limit";
        case OrderType::Market: return "market";
        case OrderType::IOC: return "ioc";
    }
    return "unknown";
}

constexpr std::string_view toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::Resting: return "resting";
        case OrderStatus::PartiallyFilled: return "partially_filled";
        case OrderStatus::Filled: return "filled";
        case OrderStatus::Cancelled: return "cancelled";
        case OrderStatus::Rejected: return "rejected";
    }
    return "unknown";
}

struct OrderRequest {
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;  // ignored for market orders
    Qty qty = 0;
    OwnerId owner = kBotOwner;
};

struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty quantity = 0;   // original size
    Qty remaining = 0;  // open size
    std::uint64_t sequence = 0;  // arrival sequence (time priority)
    OwnerId owner = kBotOwner;
};

struct Trade {
    TradeId id = 0;
    Price price = 0;
    Qty qty = 0;
    Side aggressor = Side::Buy;
    OrderId makerOrderId = 0;
    OrderId takerOrderId = 0;
    OwnerId makerOwner = kBotOwner;
    OwnerId takerOwner = kBotOwner;
    Qty makerRemaining = 0;
    std::int64_t timestampMs = 0;
};

struct ExecutionReport {
    OrderId orderId = 0;
    OrderStatus status = OrderStatus::Rejected;
    Qty filledQty = 0;
    Qty remainingQty = 0;
    std::vector<Trade> trades;
    std::string reason;

    // Volume-weighted fill price in price units (not ticks); 0 when nothing filled.
    double averagePrice() const noexcept {
        if (filledQty == 0) return 0.0;
        long double notional = 0;
        for (const auto& t : trades) notional += static_cast<long double>(t.price) * t.qty;
        return static_cast<double>(notional / filledQty / kTicksPerUnit);
    }
};

struct CancelResult {
    bool ok = false;
    std::string reason;
    Order order{};
};

struct LevelView {
    Price price = 0;
    Qty qty = 0;
    std::uint32_t orders = 0;
};

struct BookSnapshot {
    std::vector<LevelView> bids;  // best (highest) first
    std::vector<LevelView> asks;  // best (lowest) first
    std::size_t bidLevels = 0;
    std::size_t askLevels = 0;
    std::size_t orderCount = 0;
};

struct TopOfBook {
    bool hasBid = false;
    bool hasAsk = false;
    Price bestBid = 0;
    Price bestAsk = 0;
};

}  // namespace lob
