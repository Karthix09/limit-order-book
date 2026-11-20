#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "lob/OrderBook.hpp"

namespace lob {

struct EngineStats {
    std::uint64_t ordersReceived = 0;
    std::uint64_t cancelsProcessed = 0;
    std::uint64_t tradeCount = 0;
    std::uint64_t volume = 0;
    bool hasLastTrade = false;
    Price lastPrice = 0;
    Side lastAggressor = Side::Buy;
    TopOfBook top{};
    std::size_t restingOrders = 0;
    std::size_t bidLevels = 0;
    std::size_t askLevels = 0;
};

// Thread-safe facade over OrderBook. All book access is serialised by one mutex,
// which keeps matching deterministic. It also buffers executed trades for the
// publisher and keeps a bounded trade history.
class MatchingEngine {
public:
    explicit MatchingEngine(std::size_t historySize = 500, std::size_t maxPendingTrades = 20'000);

    ExecutionReport submit(const OrderRequest& request);
    CancelResult cancel(OrderId id, OwnerId requester, bool enforceOwnership);
    std::size_t cancelAllForOwner(OwnerId owner);

    BookSnapshot snapshot(std::size_t depth) const;
    TopOfBook topOfBook() const;
    EngineStats stats() const;

    // Returns and clears the trades executed since the previous call (oldest first).
    std::vector<Trade> drainPendingTrades();
    // Most recent trades, oldest first.
    std::vector<Trade> recentTrades(std::size_t maxCount) const;

private:
    void recordTrades(const std::vector<Trade>& trades);

    mutable std::mutex mutex_;
    OrderBook book_;
    std::deque<Trade> history_;
    std::vector<Trade> pending_;
    std::size_t historySize_;
    std::size_t maxPending_;
    EngineStats stats_;
};

}  // namespace lob
