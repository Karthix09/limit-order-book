#include "lob/MatchingEngine.hpp"

#include <algorithm>
#include <iterator>

namespace lob {

MatchingEngine::MatchingEngine(std::size_t historySize, std::size_t maxPendingTrades)
    : historySize_(historySize), maxPending_(maxPendingTrades) {}

ExecutionReport MatchingEngine::submit(const OrderRequest& request) {
    std::lock_guard lock(mutex_);
    ++stats_.ordersReceived;
    ExecutionReport report = book_.submit(request);
    recordTrades(report.trades);
    return report;
}

CancelResult MatchingEngine::cancel(OrderId id, OwnerId requester, bool enforceOwnership) {
    std::lock_guard lock(mutex_);
    CancelResult result;

    auto existing = book_.find(id);
    if (!existing) {
        result.reason = "order not found (already filled or cancelled)";
        return result;
    }
    if (enforceOwnership && existing->owner != requester) {
        result.reason = "order belongs to another participant";
        return result;
    }

    auto cancelled = book_.cancel(id);
    result.ok = cancelled.has_value();
    if (cancelled) {
        result.order = *cancelled;
        ++stats_.cancelsProcessed;
    }
    return result;
}

std::size_t MatchingEngine::cancelAllForOwner(OwnerId owner) {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (OrderId id : book_.ordersForOwner(owner)) {
        if (book_.cancel(id)) {
            ++count;
            ++stats_.cancelsProcessed;
        }
    }
    return count;
}

BookSnapshot MatchingEngine::snapshot(std::size_t depth) const {
    std::lock_guard lock(mutex_);
    return book_.snapshot(depth);
}

TopOfBook MatchingEngine::topOfBook() const {
    std::lock_guard lock(mutex_);
    return book_.top();
}

EngineStats MatchingEngine::stats() const {
    std::lock_guard lock(mutex_);
    EngineStats s = stats_;
    s.top = book_.top();
    s.restingOrders = book_.orderCount();
    s.bidLevels = book_.bidLevelCount();
    s.askLevels = book_.askLevelCount();
    return s;
}

std::vector<Trade> MatchingEngine::drainPendingTrades() {
    std::lock_guard lock(mutex_);
    std::vector<Trade> out;
    out.swap(pending_);
    return out;
}

std::vector<Trade> MatchingEngine::recentTrades(std::size_t maxCount) const {
    std::lock_guard lock(mutex_);
    const std::size_t n = std::min(maxCount, history_.size());
    return std::vector<Trade>(history_.end() - static_cast<std::ptrdiff_t>(n), history_.end());
}

void MatchingEngine::recordTrades(const std::vector<Trade>& trades) {
    for (const Trade& t : trades) {
        ++stats_.tradeCount;
        stats_.volume += static_cast<std::uint64_t>(t.qty);
        stats_.hasLastTrade = true;
        stats_.lastPrice = t.price;
        stats_.lastAggressor = t.aggressor;

        history_.push_back(t);
        if (history_.size() > historySize_) history_.pop_front();

        // If nobody drains the buffer it is capped; the oldest unsent trades are dropped.
        if (pending_.size() >= maxPending_) {
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(pending_.size() / 2));
        }
        pending_.push_back(t);
    }
}

}  // namespace lob
