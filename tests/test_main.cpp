// Self-contained unit tests (no framework). Run via `ctest` or ./build/lob_tests.
#include <cstdlib>
#include <iostream>
#include <string>

#include "lob/OrderBook.hpp"
#include "net/Crypto.hpp"
#include "util/Json.hpp"

namespace {

int gFailures = 0;
int gChecks = 0;

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        ++gChecks;                                                                          \
        if (!(cond)) {                                                                      \
            ++gFailures;                                                                    \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n"; \
        }                                                                                   \
    } while (0)

using namespace lob;

ExecutionReport limit(OrderBook& b, Side s, Price p, Qty q, OwnerId owner = 7) {
    return b.submit(OrderRequest{s, OrderType::Limit, p, q, owner});
}

void testRestingAndTopOfBook() {
    OrderBook b;
    auto r1 = limit(b, Side::Buy, 9990, 10);
    auto r2 = limit(b, Side::Sell, 10010, 5);
    CHECK(r1.status == OrderStatus::Resting);
    CHECK(r2.status == OrderStatus::Resting);
    auto top = b.top();
    CHECK(top.hasBid && top.bestBid == 9990);
    CHECK(top.hasAsk && top.bestAsk == 10010);
    CHECK(b.orderCount() == 2);
}

void testPriceTimePriority() {
    OrderBook b;
    auto first = limit(b, Side::Sell, 10000, 5, 1);
    auto second = limit(b, Side::Sell, 10000, 5, 2);
    auto better = limit(b, Side::Sell, 9995, 3, 3);

    auto taker = b.submit(OrderRequest{Side::Buy, OrderType::Market, 0, 10, 9});
    CHECK(taker.status == OrderStatus::Filled);
    CHECK(taker.trades.size() == 3);
    // Best price first, then FIFO within the level.
    CHECK(taker.trades[0].makerOrderId == better.orderId && taker.trades[0].price == 9995 && taker.trades[0].qty == 3);
    CHECK(taker.trades[1].makerOrderId == first.orderId && taker.trades[1].qty == 5);
    CHECK(taker.trades[2].makerOrderId == second.orderId && taker.trades[2].qty == 2);
    CHECK(taker.trades[2].makerRemaining == 3);
    CHECK(b.find(second.orderId)->remaining == 3);
    CHECK(!b.find(first.orderId).has_value());
}

void testPartialFillThenRest() {
    OrderBook b;
    limit(b, Side::Sell, 10000, 4);
    auto r = limit(b, Side::Buy, 10005, 10);
    CHECK(r.status == OrderStatus::PartiallyFilled);
    CHECK(r.filledQty == 4 && r.remainingQty == 6);
    CHECK(r.trades.size() == 1 && r.trades[0].price == 10000);  // executes at maker price
    auto top = b.top();
    CHECK(top.hasBid && top.bestBid == 10005);
    CHECK(!top.hasAsk);
    auto snap = b.snapshot(10);
    CHECK(snap.bids.size() == 1 && snap.bids[0].qty == 6 && snap.bids[0].orders == 1);
}

void testLimitDoesNotCrossBeyondPrice() {
    OrderBook b;
    limit(b, Side::Sell, 10000, 5);
    limit(b, Side::Sell, 10010, 5);
    auto r = limit(b, Side::Buy, 10005, 8);
    CHECK(r.filledQty == 5);
    CHECK(r.remainingQty == 3);
    CHECK(b.top().bestBid == 10005 && b.top().bestAsk == 10010);
}

void testIocCancelsRemainder() {
    OrderBook b;
    limit(b, Side::Buy, 10000, 5);
    auto r = b.submit(OrderRequest{Side::Sell, OrderType::IOC, 10000, 8, 9});
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK(r.filledQty == 5);
    CHECK(r.remainingQty == 0);
    CHECK(b.orderCount() == 0);
    CHECK(!b.top().hasAsk);  // IOC never rests

    auto none = b.submit(OrderRequest{Side::Sell, OrderType::IOC, 10000, 1, 9});
    CHECK(none.status == OrderStatus::Cancelled && none.filledQty == 0);
}

void testMarketOnEmptyBook() {
    OrderBook b;
    auto r = b.submit(OrderRequest{Side::Buy, OrderType::Market, 0, 10, 9});
    CHECK(r.status == OrderStatus::Cancelled);
    CHECK(r.filledQty == 0);
    CHECK(r.reason == "no liquidity");
}

void testCancel() {
    OrderBook b;
    auto a = limit(b, Side::Buy, 10000, 5);
    auto c = limit(b, Side::Buy, 10000, 7);
    auto d = limit(b, Side::Buy, 9990, 2);

    auto cancelled = b.cancel(a.orderId);
    CHECK(cancelled && cancelled->remaining == 5);
    auto snap = b.snapshot(10);
    CHECK(snap.bids.size() == 2 && snap.bids[0].qty == 7 && snap.bids[0].orders == 1);

    CHECK(b.cancel(c.orderId).has_value());
    CHECK(b.top().bestBid == 9990);  // emptied level removed
    CHECK(!b.cancel(c.orderId).has_value());  // double cancel
    CHECK(!b.cancel(424242).has_value());
    CHECK(b.cancel(d.orderId).has_value());
    CHECK(b.orderCount() == 0 && b.bidLevelCount() == 0);
}

void testCancelAfterPartialFill() {
    OrderBook b;
    auto maker = limit(b, Side::Sell, 10000, 10);
    b.submit(OrderRequest{Side::Buy, OrderType::Market, 0, 4, 9});
    auto c = b.cancel(maker.orderId);
    CHECK(c && c->remaining == 6 && c->quantity == 10);
    CHECK(b.askLevelCount() == 0);
}

void testValidation() {
    OrderBook b;
    CHECK(limit(b, Side::Buy, 10000, 0).status == OrderStatus::Rejected);
    CHECK(limit(b, Side::Buy, 0, 5).status == OrderStatus::Rejected);
    CHECK(limit(b, Side::Buy, -5, 5).status == OrderStatus::Rejected);
    CHECK(limit(b, Side::Buy, 10000, kMaxOrderQty + 1).status == OrderStatus::Rejected);
    CHECK(b.orderCount() == 0);
}

void testOwnerLookup() {
    OrderBook b;
    limit(b, Side::Buy, 9900, 1, 5);
    limit(b, Side::Sell, 10100, 1, 5);
    limit(b, Side::Sell, 10200, 1, 6);
    CHECK(b.ordersForOwner(5).size() == 2);
    CHECK(b.ordersForOwner(6).size() == 1);
    CHECK(b.ordersForOwner(99).empty());
}

void testAveragePrice() {
    OrderBook b;
    limit(b, Side::Sell, 10000, 1);
    limit(b, Side::Sell, 10002, 1);
    auto r = b.submit(OrderRequest{Side::Buy, OrderType::Market, 0, 2, 9});
    CHECK(r.averagePrice() > 100.0099 && r.averagePrice() < 100.0101);
}

void testWebSocketAccept() {
    // Example from RFC 6455 section 1.3.
    CHECK(net::webSocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    const auto d = net::sha1("abc");
    CHECK(net::base64Encode(d.data(), d.size()) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
}

void testJson() {
    auto r = json::parseFlatObject(R"({"action":"order","side":"buy","price":101.25,"qty":"10","ok":true,"x":null,"s":"a\"bé"})");
    CHECK(r.object.has_value());
    if (r.object) {
        CHECK(r.object->getString("action") == "order");
        CHECK(r.object->getNumber("price") == 101.25);
        CHECK(r.object->getNumber("qty") == 10.0);
        CHECK(r.object->getBool("ok") == true);
        CHECK(r.object->has("x"));
        CHECK(r.object->getString("s") == std::string("a\"b\xC3\xA9"));
    }
    CHECK(!json::parseFlatObject("{\"a\":[1]}").object.has_value());
    CHECK(!json::parseFlatObject("{\"a\":1,}").object.has_value());
    CHECK(!json::parseFlatObject("not json").object.has_value());
    CHECK(json::quote("a\"b\n") == "\"a\\\"b\\n\"");
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"resting & top of book", testRestingAndTopOfBook},
        {"price-time priority", testPriceTimePriority},
        {"partial fill then rest", testPartialFillThenRest},
        {"limit respects price", testLimitDoesNotCrossBeyondPrice},
        {"IOC cancels remainder", testIocCancelsRemainder},
        {"market on empty book", testMarketOnEmptyBook},
        {"cancel", testCancel},
        {"cancel after partial fill", testCancelAfterPartialFill},
        {"validation", testValidation},
        {"owner lookup", testOwnerLookup},
        {"average price", testAveragePrice},
        {"websocket accept key", testWebSocketAccept},
        {"json", testJson},
    };
    for (const auto& t : tests) {
        const int before = gFailures;
        t.fn();
        std::cout << (gFailures == before ? "[ OK ] " : "[FAIL] ") << t.name << "\n";
    }
    std::cout << gChecks << " checks, " << gFailures << " failure(s)\n";
    return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
