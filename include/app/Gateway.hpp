#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lob/MarketMaker.hpp"
#include "lob/MatchingEngine.hpp"
#include "net/WebServer.hpp"
#include "util/Json.hpp"

namespace app {

struct GatewayConfig {
    std::size_t bookDepth = 25;
    std::chrono::milliseconds publishInterval{100};
    std::size_t initialTrades = 100;
};

// Wires the matching engine and market-maker bot to the network:
//   * Parses client commands (WebSocket or REST) and returns JSON acks.
//   * Publishes book snapshots, new trades and stats to all clients at a fixed rate.
//   * Routes passive (maker) fills back to the owning WebSocket client.
//   * Cancels a client's resting orders when it disconnects.
class Gateway {
public:
    Gateway(lob::MatchingEngine& engine, lob::MarketMaker& bot, net::WebServer& server, GatewayConfig config);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    void attach();  // install server handlers; call before WebServer::start()
    void start();   // start the publisher thread
    void stop();

private:
    void onOpen(net::ClientId client);
    void onClose(net::ClientId client);
    void onMessage(net::ClientId client, const std::string& text);
    std::optional<net::HttpResponse> onHttp(const net::HttpRequest& request);

    std::string handleCommand(const json::Object& cmd, lob::OwnerId owner, bool enforceOwnership);
    std::string handleOrder(const json::Object& cmd, lob::OwnerId owner, const std::string& reqId);
    std::string handleCancel(const json::Object& cmd, lob::OwnerId owner, bool enforceOwnership,
                             const std::string& reqId);
    std::string handleBot(const json::Object& cmd, const std::string& reqId);

    std::string bookJson(const lob::BookSnapshot& snap) const;
    std::string statsJson(const lob::EngineStats& stats, double ordersPerSec) const;
    std::string botJson() const;

    void publishLoop();

    lob::MatchingEngine& engine_;
    lob::MarketMaker& bot_;
    net::WebServer& server_;
    GatewayConfig config_;

    std::thread publisher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<double> ordersPerSec_{0.0};
};

}  // namespace app
