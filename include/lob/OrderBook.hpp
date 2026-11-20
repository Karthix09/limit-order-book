#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/Types.hpp"

namespace lob {

// Single-threaded limit order book with strict price-time (FIFO) priority.
//
// Layout:
//   * Each price level holds a FIFO std::list<Order>; the front is the oldest order.
//   * Bids live in a std::map ordered descending, asks in a std::map ordered ascending,
//     so begin() is always the best price on either side.
//   * index_ maps OrderId -> {level iterator, list iterator}. std::map and std::list
//     iterators stay valid across unrelated inserts/erases, so lookup and cancellation
//     are O(1) (erasing an emptied level by iterator is amortised O(1)).
class OrderBook {
public:
    ExecutionReport submit(const OrderRequest& request);
    std::optional<Order> cancel(OrderId id);

    std::optional<Order> find(OrderId id) const;
    std::vector<OrderId> ordersForOwner(OwnerId owner) const;

    BookSnapshot snapshot(std::size_t depth) const;
    TopOfBook top() const;

    std::size_t orderCount() const noexcept { return index_.size(); }
    std::size_t bidLevelCount() const noexcept { return bids_.size(); }
    std::size_t askLevelCount() const noexcept { return asks_.size(); }

private:
    using OrderList = std::list<Order>;

    struct Level {
        Qty totalQty = 0;
        OrderList orders;
    };

    using BidMap = std::map<Price, Level, std::greater<Price>>;
    using AskMap = std::map<Price, Level, std::less<Price>>;

    struct Locator {
        Side side = Side::Buy;
        BidMap::iterator bidLevel{};
        AskMap::iterator askLevel{};
        OrderList::iterator order{};
    };

    template <class BookSide, class CrossesFn>
    void matchAgainst(BookSide& book, Order& taker, CrossesFn crosses, ExecutionReport& report,
                      std::int64_t timestampMs);

    void rest(const Order& order);

    template <class BookSide>
    static void collectLevels(const BookSide& book, std::size_t depth, std::vector<LevelView>& out);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Locator> index_;

    OrderId nextOrderId_ = 1;
    TradeId nextTradeId_ = 1;
    std::uint64_t nextSequence_ = 1;
};

}  // namespace lob
