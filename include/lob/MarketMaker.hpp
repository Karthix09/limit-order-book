#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "lob/MatchingEngine.hpp"

namespace lob {

struct MarketMakerConfig {
    Price initialMid = 100 * kTicksPerUnit;  // 100.00
    int actionsPerSecond = 40;
    bool startRunning = true;
    std::size_t targetRestingOrders = 250;
    int seedLevels = 15;
    double volatilityTicks = 0.35;  // std-dev of the fair-value random walk per action
    std::uint64_t seed = 0;         // 0 = seed from std::random_device
};

// Background order-flow generator. A latent "fair value" follows a random walk
// (with mild reversion towards the live book mid so manual sweeps are absorbed),
// and the bot emits passive limit orders around it, cancels stale quotes and
// occasionally sends aggressive market / IOC orders.
class MarketMaker {
public:
    MarketMaker(MatchingEngine& engine, MarketMakerConfig config);
    ~MarketMaker();

    MarketMaker(const MarketMaker&) = delete;
    MarketMaker& operator=(const MarketMaker&) = delete;

    void start();
    void stop();

    void setRunning(bool running);
    bool running() const noexcept { return running_.load(); }

    void setRate(int actionsPerSecond);
    int rate() const noexcept { return rate_.load(); }

    static constexpr int kMinRate = 1;
    static constexpr int kMaxRate = 2000;

private:
    void run();
    void seedBook();
    void step();

    void placePassive(Side side, Price price, Qty qty);
    void cancelOne();
    void sendAggressive(const TopOfBook& top);

    Qty passiveQty();
    Qty aggressiveQty();
    int passiveOffset();
    double uniform();

    MatchingEngine& engine_;
    MarketMakerConfig config_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{true};
    std::atomic<int> rate_{40};
    bool started_ = false;

    // Bot-thread-only state.
    std::mt19937_64 rng_;
    double fair_ = 0.0;  // in ticks
    std::vector<std::pair<OrderId, Price>> live_;
};

}  // namespace lob
