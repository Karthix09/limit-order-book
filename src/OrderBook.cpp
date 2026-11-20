#include "lob/OrderBook.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>

namespace lob {

namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

ExecutionReport OrderBook::submit(const OrderRequest& request) {
    ExecutionReport report;

    if (request.qty <= 0) {
        report.reason = "quantity must be positive";
        return report;
    }
    if (request.qty > kMaxOrderQty) {
        report.reason = "quantity exceeds maximum";
        return report;
    }
    if (request.type != OrderType::Market && (request.price <= 0 || request.price > kMaxPrice)) {
        report.reason = "price out of range";
        return report;
    }

    Order taker;
    taker.id = nextOrderId_++;
    taker.side = request.side;
    taker.price = request.type == OrderType::Market ? 0 : request.price;
    taker.quantity = request.qty;
    taker.remaining = request.qty;
    taker.sequence = nextSequence_++;
    taker.owner = request.owner;

    report.orderId = taker.id;
    const std::int64_t ts = nowMs();
    const bool isMarket = request.type == OrderType::Market;

    if (taker.side == Side::Buy) {
        matchAgainst(asks_, taker, [&](Price ask) { return isMarket || ask <= taker.price; }, report, ts);
    } else {
        matchAgainst(bids_, taker, [&](Price bid) { return isMarket || bid >= taker.price; }, report, ts);
    }

    report.filledQty = taker.quantity - taker.remaining;

    if (taker.remaining == 0) {
        report.status = OrderStatus::Filled;
        report.remainingQty = 0;
    } else if (request.type == OrderType::Limit) {
        rest(taker);
        report.status = report.filledQty > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
        report.remainingQty = taker.remaining;
    } else {
        // Market and IOC never rest: the unfilled remainder is cancelled.
        report.status = OrderStatus::Cancelled;
        report.remainingQty = 0;
        if (report.filledQty == 0) {
            report.reason = isMarket ? "no liquidity" : "no liquidity at limit price";
        } else {
            report.reason = "unfilled remainder cancelled";
        }
    }
    return report;
}

template <class BookSide, class CrossesFn>
void OrderBook::matchAgainst(BookSide& book, Order& taker, CrossesFn crosses, ExecutionReport& report,
                             std::int64_t timestampMs) {
    while (taker.remaining > 0 && !book.empty()) {
        auto levelIt = book.begin();
        if (!crosses(levelIt->first)) break;

        Level& level = levelIt->second;
        while (taker.remaining > 0 && !level.orders.empty()) {
            Order& maker = level.orders.front();
            const Qty fill = std::min(taker.remaining, maker.remaining);

            taker.remaining -= fill;
            maker.remaining -= fill;
            level.totalQty -= fill;

            Trade trade;
            trade.id = nextTradeId_++;
            trade.price = levelIt->first;  // trades print at the resting (maker) price
            trade.qty = fill;
            trade.aggressor = taker.side;
            trade.makerOrderId = maker.id;
            trade.takerOrderId = taker.id;
            trade.makerOwner = maker.owner;
            trade.takerOwner = taker.owner;
            trade.makerRemaining = maker.remaining;
            trade.timestampMs = timestampMs;
            report.trades.push_back(trade);

            if (maker.remaining == 0) {
                index_.erase(maker.id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) book.erase(levelIt);
    }
}

void OrderBook::rest(const Order& order) {
    Locator loc;
    loc.side = order.side;
    if (order.side == Side::Buy) {
        auto [levelIt, inserted] = bids_.try_emplace(order.price);
        (void)inserted;
        levelIt->second.orders.push_back(order);
        levelIt->second.totalQty += order.remaining;
        loc.bidLevel = levelIt;
        loc.order = std::prev(levelIt->second.orders.end());
    } else {
        auto [levelIt, inserted] = asks_.try_emplace(order.price);
        (void)inserted;
        levelIt->second.orders.push_back(order);
        levelIt->second.totalQty += order.remaining;
        loc.askLevel = levelIt;
        loc.order = std::prev(levelIt->second.orders.end());
    }
    index_.emplace(order.id, loc);
}

std::optional<Order> OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return std::nullopt;

    const Locator loc = found->second;
    Order cancelled = *loc.order;

    if (loc.side == Side::Buy) {
        Level& level = loc.bidLevel->second;
        level.totalQty -= cancelled.remaining;
        level.orders.erase(loc.order);
        if (level.orders.empty()) bids_.erase(loc.bidLevel);
    } else {
        Level& level = loc.askLevel->second;
        level.totalQty -= cancelled.remaining;
        level.orders.erase(loc.order);
        if (level.orders.empty()) asks_.erase(loc.askLevel);
    }
    index_.erase(found);
    return cancelled;
}

std::optional<Order> OrderBook::find(OrderId id) const {
    auto found = index_.find(id);
    if (found == index_.end()) return std::nullopt;
    return *found->second.order;
}

std::vector<OrderId> OrderBook::ordersForOwner(OwnerId owner) const {
    std::vector<OrderId> ids;
    for (const auto& [id, loc] : index_) {
        if (loc.order->owner == owner) ids.push_back(id);
    }
    return ids;
}

template <class BookSide>
void OrderBook::collectLevels(const BookSide& book, std::size_t depth, std::vector<LevelView>& out) {
    out.reserve(std::min(depth, book.size()));
    for (auto it = book.begin(); it != book.end() && out.size() < depth; ++it) {
        out.push_back(LevelView{it->first, it->second.totalQty,
                                static_cast<std::uint32_t>(it->second.orders.size())});
    }
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
    BookSnapshot snap;
    collectLevels(bids_, depth, snap.bids);
    collectLevels(asks_, depth, snap.asks);
    snap.bidLevels = bids_.size();
    snap.askLevels = asks_.size();
    snap.orderCount = index_.size();
    return snap;
}

TopOfBook OrderBook::top() const {
    TopOfBook t;
    if (!bids_.empty()) {
        t.hasBid = true;
        t.bestBid = bids_.begin()->first;
    }
    if (!asks_.empty()) {
        t.hasAsk = true;
        t.bestAsk = asks_.begin()->first;
    }
    return t;
}

}  // namespace lob
