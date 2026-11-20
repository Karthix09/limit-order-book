#include "app/Gateway.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>

namespace app {

namespace {

using lob::kTicksPerUnit;

std::string priceStr(lob::Price ticks) {
    const bool negative = ticks < 0;
    const auto abs = static_cast<unsigned long long>(negative ? -ticks : ticks);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%llu.%02llu", negative ? "-" : "", abs / kTicksPerUnit, abs % kTicksPerUnit);
    return buf;
}

std::string numStr(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string tradeJson(const lob::Trade& t) {
    std::string s = "{\"id\":" + std::to_string(t.id);
    s += ",\"price\":" + priceStr(t.price);
    s += ",\"qty\":" + std::to_string(t.qty);
    s += ",\"side\":\"" + std::string(lob::toString(t.aggressor)) + "\"";
    s += ",\"ts\":" + std::to_string(t.timestampMs) + "}";
    return s;
}

std::string tradesJson(const std::vector<lob::Trade>& trades) {
    std::string s = "[";
    for (std::size_t i = 0; i < trades.size(); ++i) {
        if (i) s += ',';
        s += tradeJson(trades[i]);
    }
    s += ']';
    return s;
}

std::string levelsJson(const std::vector<lob::LevelView>& levels) {
    std::string s = "[";
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i) s += ',';
        s += '[' + priceStr(levels[i].price) + ',' + std::to_string(levels[i].qty) + ',' +
             std::to_string(levels[i].orders) + ']';
    }
    s += ']';
    return s;
}

std::string errorJson(const std::string& action, const std::string& reqId, const std::string& reason) {
    return "{\"type\":\"ack\",\"action\":" + json::quote(action) + ",\"reqId\":" + reqId +
           ",\"ok\":false,\"reason\":" + json::quote(reason) + "}";
}

bool isClientOwner(lob::OwnerId owner) { return owner != lob::kBotOwner && owner != lob::kRestOwner; }

std::size_t queryInt(const std::string& query, const std::string& key, std::size_t fallback, std::size_t maxValue) {
    const std::string needle = key + "=";
    std::size_t pos = 0;
    while ((pos = query.find(needle, pos)) != std::string::npos) {
        if (pos == 0 || query[pos - 1] == '&') {
            try {
                const auto v = static_cast<std::size_t>(std::stoul(query.substr(pos + needle.size())));
                return std::min(std::max<std::size_t>(v, 1), maxValue);
            } catch (...) {
                return fallback;
            }
        }
        pos += needle.size();
    }
    return fallback;
}

}  // namespace

Gateway::Gateway(lob::MatchingEngine& engine, lob::MarketMaker& bot, net::WebServer& server, GatewayConfig config)
    : engine_(engine), bot_(bot), server_(server), config_(config) {}

Gateway::~Gateway() { stop(); }

void Gateway::attach() {
    server_.setOpenHandler([this](net::ClientId c) { onOpen(c); });
    server_.setCloseHandler([this](net::ClientId c) { onClose(c); });
    server_.setMessageHandler([this](net::ClientId c, const std::string& m) { onMessage(c, m); });
    server_.setHttpHandler([this](const net::HttpRequest& r) { return onHttp(r); });
}

void Gateway::start() {
    if (running_.exchange(true)) return;
    publisher_ = std::thread(&Gateway::publishLoop, this);
}

void Gateway::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_.exchange(false)) return;
    }
    cv_.notify_all();
    if (publisher_.joinable()) publisher_.join();
}

void Gateway::onOpen(net::ClientId client) {
    std::string hello = "{\"type\":\"hello\",\"clientId\":" + std::to_string(client) +
                        ",\"tickSize\":" + priceStr(1) + ",\"bot\":" + botJson() +
                        ",\"book\":" + bookJson(engine_.snapshot(config_.bookDepth)) +
                        ",\"trades\":" + tradesJson(engine_.recentTrades(config_.initialTrades)) + "}";
    server_.send(client, hello);
}

void Gateway::onClose(net::ClientId client) {
    const std::size_t n = engine_.cancelAllForOwner(client);
    if (n > 0) std::cout << "[gateway] client " << client << " disconnected; cancelled " << n << " order(s)\n";
}

void Gateway::onMessage(net::ClientId client, const std::string& text) {
    auto parsed = json::parseFlatObject(text);
    if (!parsed.object) {
        server_.send(client, errorJson("unknown", "null", "invalid JSON: " + parsed.error));
        return;
    }
    server_.send(client, handleCommand(*parsed.object, client, true));
}

std::optional<net::HttpResponse> Gateway::onHttp(const net::HttpRequest& request) {
    if (request.path.rfind("/api/", 0) != 0) return std::nullopt;

    net::HttpResponse response;
    auto runCommand = [&](const std::string& action) {
        if (request.method != "POST") {
            response.status = 405;
            response.body = errorJson(action, "null", "use POST");
            return;
        }
        auto parsed = json::parseFlatObject(request.body.empty() ? "{}" : request.body);
        if (!parsed.object) {
            response.status = 400;
            response.body = errorJson(action, "null", "invalid JSON: " + parsed.error);
            return;
        }
        parsed.object->fields["action"] = json::Value{json::Value::Type::String, false, 0.0, action};
        // REST callers act as an admin participant: they may cancel any order by ID.
        response.body = handleCommand(*parsed.object, lob::kRestOwner, false);
        if (response.body.find("\"ok\":false") != std::string::npos) response.status = 400;
    };

    if (request.path == "/api/health") {
        response.body = "{\"ok\":true,\"clients\":" + std::to_string(server_.clientCount()) + "}";
    } else if (request.path == "/api/book") {
        const std::size_t depth = queryInt(request.query, "depth", config_.bookDepth, 500);
        response.body = "{\"book\":" + bookJson(engine_.snapshot(depth)) +
                        ",\"stats\":" + statsJson(engine_.stats(), ordersPerSec_.load()) + "}";
    } else if (request.path == "/api/trades") {
        const std::size_t limit = queryInt(request.query, "limit", 100, 500);
        response.body = "{\"trades\":" + tradesJson(engine_.recentTrades(limit)) + "}";
    } else if (request.path == "/api/order") {
        runCommand("order");
    } else if (request.path == "/api/cancel") {
        runCommand("cancel");
    } else if (request.path == "/api/bot") {
        if (request.method == "GET") {
            response.body = botJson();
        } else {
            runCommand("bot");
        }
    } else {
        response.status = 404;
        response.body = "{\"ok\":false,\"reason\":\"unknown endpoint\"}";
    }
    return response;
}

std::string Gateway::handleCommand(const json::Object& cmd, lob::OwnerId owner, bool enforceOwnership) {
    std::string reqId = "null";
    if (auto id = cmd.getNumber("reqId")) reqId = std::to_string(static_cast<long long>(*id));

    const std::string action = cmd.getString("action").value_or("");
    if (action == "order") return handleOrder(cmd, owner, reqId);
    if (action == "cancel") return handleCancel(cmd, owner, enforceOwnership, reqId);
    if (action == "bot") return handleBot(cmd, reqId);
    return errorJson(action.empty() ? "unknown" : action, reqId, "unknown action (expected order|cancel|bot)");
}

std::string Gateway::handleOrder(const json::Object& cmd, lob::OwnerId owner, const std::string& reqId) {
    lob::OrderRequest req;
    req.owner = owner;

    const std::string side = cmd.getString("side").value_or("");
    if (side == "buy") req.side = lob::Side::Buy;
    else if (side == "sell") req.side = lob::Side::Sell;
    else return errorJson("order", reqId, "side must be 'buy' or 'sell'");

    const std::string type = cmd.getString("type").value_or("limit");
    if (type == "limit") req.type = lob::OrderType::Limit;
    else if (type == "market") req.type = lob::OrderType::Market;
    else if (type == "ioc") req.type = lob::OrderType::IOC;
    else return errorJson("order", reqId, "type must be 'limit', 'market' or 'ioc'");

    const auto qty = cmd.getNumber("qty");
    if (!qty || !std::isfinite(*qty) || *qty <= 0 || std::floor(*qty) != *qty) {
        return errorJson("order", reqId, "qty must be a positive integer");
    }
    if (*qty > static_cast<double>(lob::kMaxOrderQty)) return errorJson("order", reqId, "qty exceeds maximum");
    req.qty = static_cast<lob::Qty>(*qty);

    if (req.type != lob::OrderType::Market) {
        const auto price = cmd.getNumber("price");
        if (!price || !std::isfinite(*price) || *price <= 0) {
            return errorJson("order", reqId, "price must be a positive number");
        }
        const double ticks = *price * static_cast<double>(kTicksPerUnit);
        if (ticks > static_cast<double>(lob::kMaxPrice)) return errorJson("order", reqId, "price out of range");
        if (std::fabs(ticks - std::round(ticks)) > 1e-6) {
            return errorJson("order", reqId, "price must be a multiple of the tick size " + priceStr(1));
        }
        req.price = static_cast<lob::Price>(std::llround(ticks));
    }

    const lob::ExecutionReport rep = engine_.submit(req);
    const bool ok = rep.status != lob::OrderStatus::Rejected;

    std::string s = "{\"type\":\"ack\",\"action\":\"order\",\"reqId\":" + reqId;
    s += ",\"ok\":" + std::string(ok ? "true" : "false");
    s += ",\"orderId\":" + std::to_string(rep.orderId);
    s += ",\"side\":\"" + std::string(lob::toString(req.side)) + "\"";
    s += ",\"orderType\":\"" + std::string(lob::toString(req.type)) + "\"";
    s += ",\"price\":" + (req.type == lob::OrderType::Market ? std::string("null") : priceStr(req.price));
    s += ",\"qty\":" + std::to_string(req.qty);
    s += ",\"status\":\"" + std::string(lob::toString(rep.status)) + "\"";
    s += ",\"filledQty\":" + std::to_string(rep.filledQty);
    s += ",\"remainingQty\":" + std::to_string(rep.remainingQty);
    s += ",\"avgPrice\":" + (rep.filledQty > 0 ? numStr(rep.averagePrice(), 4) : std::string("null"));
    s += ",\"reason\":" + json::quote(rep.reason);
    s += ",\"fills\":[";
    for (std::size_t i = 0; i < rep.trades.size(); ++i) {
        if (i) s += ',';
        s += "{\"price\":" + priceStr(rep.trades[i].price) + ",\"qty\":" + std::to_string(rep.trades[i].qty) + "}";
    }
    s += "]}";
    return s;
}

std::string Gateway::handleCancel(const json::Object& cmd, lob::OwnerId owner, bool enforceOwnership,
                                  const std::string& reqId) {
    const auto id = cmd.getNumber("orderId");
    if (!id || *id <= 0 || std::floor(*id) != *id) return errorJson("cancel", reqId, "orderId must be a positive integer");

    const auto orderId = static_cast<lob::OrderId>(*id);
    const lob::CancelResult res = engine_.cancel(orderId, owner, enforceOwnership);
    if (!res.ok) {
        return "{\"type\":\"ack\",\"action\":\"cancel\",\"reqId\":" + reqId + ",\"ok\":false,\"orderId\":" +
               std::to_string(orderId) + ",\"reason\":" + json::quote(res.reason) + "}";
    }
    return "{\"type\":\"ack\",\"action\":\"cancel\",\"reqId\":" + reqId + ",\"ok\":true,\"orderId\":" +
           std::to_string(orderId) + ",\"side\":\"" + std::string(lob::toString(res.order.side)) +
           "\",\"price\":" + priceStr(res.order.price) +
           ",\"cancelledQty\":" + std::to_string(res.order.remaining) + ",\"reason\":\"\"}";
}

std::string Gateway::handleBot(const json::Object& cmd, const std::string& reqId) {
    if (auto running = cmd.getBool("running")) bot_.setRunning(*running);
    if (auto rate = cmd.getNumber("rate")) {
        if (!std::isfinite(*rate)) return errorJson("bot", reqId, "rate must be a number");
        bot_.setRate(static_cast<int>(std::lround(*rate)));
    }
    return "{\"type\":\"ack\",\"action\":\"bot\",\"reqId\":" + reqId + ",\"ok\":true,\"bot\":" + botJson() + "}";
}

std::string Gateway::bookJson(const lob::BookSnapshot& snap) const {
    return "{\"bids\":" + levelsJson(snap.bids) + ",\"asks\":" + levelsJson(snap.asks) +
           ",\"bidLevels\":" + std::to_string(snap.bidLevels) + ",\"askLevels\":" + std::to_string(snap.askLevels) +
           ",\"orders\":" + std::to_string(snap.orderCount) + "}";
}

std::string Gateway::statsJson(const lob::EngineStats& st, double ordersPerSec) const {
    const auto& top = st.top;
    std::string s = "{";
    s += "\"lastPrice\":" + (st.hasLastTrade ? priceStr(st.lastPrice) : std::string("null"));
    s += ",\"lastSide\":" + (st.hasLastTrade ? "\"" + std::string(lob::toString(st.lastAggressor)) + "\"" : std::string("null"));
    s += ",\"bestBid\":" + (top.hasBid ? priceStr(top.bestBid) : std::string("null"));
    s += ",\"bestAsk\":" + (top.hasAsk ? priceStr(top.bestAsk) : std::string("null"));
    if (top.hasBid && top.hasAsk) {
        s += ",\"spread\":" + priceStr(top.bestAsk - top.bestBid);
        s += ",\"mid\":" + numStr(static_cast<double>(top.bestBid + top.bestAsk) / (2.0 * kTicksPerUnit), 3);
    } else {
        s += ",\"spread\":null,\"mid\":null";
    }
    s += ",\"trades\":" + std::to_string(st.tradeCount);
    s += ",\"volume\":" + std::to_string(st.volume);
    s += ",\"ordersReceived\":" + std::to_string(st.ordersReceived);
    s += ",\"cancels\":" + std::to_string(st.cancelsProcessed);
    s += ",\"restingOrders\":" + std::to_string(st.restingOrders);
    s += ",\"bidLevels\":" + std::to_string(st.bidLevels);
    s += ",\"askLevels\":" + std::to_string(st.askLevels);
    s += ",\"ordersPerSec\":" + numStr(ordersPerSec, 1);
    s += ",\"clients\":" + std::to_string(server_.clientCount());
    s += "}";
    return s;
}

std::string Gateway::botJson() const {
    return "{\"running\":" + std::string(bot_.running() ? "true" : "false") + ",\"rate\":" +
           std::to_string(bot_.rate()) + ",\"minRate\":" + std::to_string(lob::MarketMaker::kMinRate) +
           ",\"maxRate\":" + std::to_string(lob::MarketMaker::kMaxRate) + "}";
}

void Gateway::publishLoop() {
    using clock = std::chrono::steady_clock;
    std::uint64_t seq = 0;
    std::uint64_t lastOrders = engine_.stats().ordersReceived;
    auto lastTime = clock::now();
    double ordersPerSec = 0.0;

    while (running_) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, config_.publishInterval, [&] { return !running_.load(); });
        }
        if (!running_) break;

        // Always drain so the pending buffer cannot grow while nobody is connected.
        const std::vector<lob::Trade> trades = engine_.drainPendingTrades();
        const lob::EngineStats stats = engine_.stats();

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTime).count();
        if (elapsed >= 0.5) {
            const double instant = static_cast<double>(stats.ordersReceived - lastOrders) / elapsed;
            ordersPerSec = ordersPerSec == 0.0 ? instant : 0.6 * ordersPerSec + 0.4 * instant;
            lastOrders = stats.ordersReceived;
            lastTime = now;
            ordersPerSec_ = ordersPerSec;
        }

        if (server_.clientCount() == 0) continue;

        // Passive fills go privately to the client that owns the resting order.
        for (const lob::Trade& t : trades) {
            if (!isClientOwner(t.makerOwner)) continue;
            const std::string fill = "{\"type\":\"fill\",\"orderId\":" + std::to_string(t.makerOrderId) +
                                     ",\"side\":\"" + std::string(lob::toString(lob::opposite(t.aggressor))) +
                                     "\",\"price\":" + priceStr(t.price) + ",\"qty\":" + std::to_string(t.qty) +
                                     ",\"remaining\":" + std::to_string(t.makerRemaining) +
                                     ",\"ts\":" + std::to_string(t.timestampMs) + "}";
            server_.send(t.makerOwner, fill);
        }

        const std::string update = "{\"type\":\"update\",\"seq\":" + std::to_string(++seq) +
                                   ",\"book\":" + bookJson(engine_.snapshot(config_.bookDepth)) +
                                   ",\"trades\":" + tradesJson(trades) + ",\"stats\":" + statsJson(stats, ordersPerSec) +
                                   ",\"bot\":" + botJson() + "}";
        server_.broadcast(update);
    }
}

}  // namespace app
