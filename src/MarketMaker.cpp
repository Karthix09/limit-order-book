#include "lob/MarketMaker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lob {

namespace {
constexpr double kMinFairTicks = 1.0 * kTicksPerUnit;  // keep prices >= 1.00
}

MarketMaker::MarketMaker(MatchingEngine& engine, MarketMakerConfig config)
    : engine_(engine), config_(config) {
    running_ = config_.startRunning;
    rate_ = std::clamp(config_.actionsPerSecond, kMinRate, kMaxRate);
    const std::uint64_t seed = config_.seed != 0 ? config_.seed : std::random_device{}();
    rng_.seed(seed);
    fair_ = static_cast<double>(config_.initialMid);
}

MarketMaker::~MarketMaker() { stop(); }

void MarketMaker::start() {
    if (started_) return;
    started_ = true;
    stop_ = false;
    thread_ = std::thread(&MarketMaker::run, this);
}

void MarketMaker::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

void MarketMaker::setRunning(bool running) {
    {
        std::lock_guard lock(mutex_);
        running_ = running;
    }
    cv_.notify_all();
}

void MarketMaker::setRate(int actionsPerSecond) {
    rate_ = std::clamp(actionsPerSecond, kMinRate, kMaxRate);
    cv_.notify_all();
}

void MarketMaker::run() {
    using clock = std::chrono::steady_clock;

    seedBook();
    auto next = clock::now();

    while (!stop_) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_.load() || running_.load(); });
        }
        if (stop_) break;

        step();

        next += std::chrono::microseconds(1'000'000 / rate_.load());
        const auto now = clock::now();
        if (next < now - std::chrono::milliseconds(100)) next = now;  // no burst after a pause

        std::unique_lock lock(mutex_);
        cv_.wait_until(lock, next, [&] { return stop_.load(); });
    }
}

void MarketMaker::seedBook() {
    const Price mid = static_cast<Price>(std::llround(fair_));
    std::uniform_int_distribution<int> ordersPerLevel(1, 3);
    for (int level = 1; level <= config_.seedLevels; ++level) {
        for (Side side : {Side::Buy, Side::Sell}) {
            const Price px = side == Side::Buy ? mid - level : mid + level;
            const int n = ordersPerLevel(rng_);
            for (int i = 0; i < n; ++i) placePassive(side, px, passiveQty());
        }
    }
}

void MarketMaker::step() {
    const TopOfBook top = engine_.topOfBook();

    // Fair value: Gaussian random walk plus weak reversion towards the observed mid.
    std::normal_distribution<double> shock(0.0, config_.volatilityTicks);
    fair_ += shock(rng_);
    if (top.hasBid && top.hasAsk) {
        const double mid = 0.5 * static_cast<double>(top.bestBid + top.bestAsk);
        fair_ += 0.02 * (mid - fair_);
    }
    fair_ = std::max(fair_, kMinFairTicks);
    const Price fairTicks = static_cast<Price>(std::llround(fair_));

    // Never let a side of the book go empty.
    if (!top.hasBid) {
        placePassive(Side::Buy, std::max<Price>(1, fairTicks - 1), passiveQty());
        return;
    }
    if (!top.hasAsk) {
        placePassive(Side::Sell, fairTicks + 1, passiveQty());
        return;
    }

    const double r = uniform();
    if (r < 0.55) {
        const Side side = uniform() < 0.5 ? Side::Buy : Side::Sell;
        const int offset = passiveOffset();
        const Price px = side == Side::Buy ? std::max<Price>(1, fairTicks - offset) : fairTicks + offset;
        placePassive(side, px, passiveQty());
    } else if (r < 0.88) {
        cancelOne();
    } else {
        sendAggressive(top);
    }

    while (live_.size() > config_.targetRestingOrders) cancelOne();
}

void MarketMaker::placePassive(Side side, Price price, Qty qty) {
    OrderRequest req;
    req.side = side;
    req.type = OrderType::Limit;
    req.price = price;
    req.qty = qty;
    req.owner = kBotOwner;
    const ExecutionReport rep = engine_.submit(req);
    if (rep.status == OrderStatus::Resting || rep.status == OrderStatus::PartiallyFilled) {
        live_.emplace_back(rep.orderId, price);
    }
}

void MarketMaker::cancelOne() {
    if (live_.empty()) return;

    std::size_t idx = 0;
    if (uniform() < 0.5) {
        // Pull the quote that has drifted furthest from fair value.
        double worst = -1.0;
        for (std::size_t i = 0; i < live_.size(); ++i) {
            const double dist = std::abs(static_cast<double>(live_[i].second) - fair_);
            if (dist > worst) {
                worst = dist;
                idx = i;
            }
        }
    } else {
        std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
        idx = pick(rng_);
    }

    const OrderId id = live_[idx].first;
    live_[idx] = live_.back();
    live_.pop_back();
    // A failed cancel just means the order already traded away.
    engine_.cancel(id, kBotOwner, true);
}

void MarketMaker::sendAggressive(const TopOfBook& top) {
    const double mid = 0.5 * static_cast<double>(top.bestBid + top.bestAsk);
    // Lean in the direction fair value is pulling the market.
    const double buyProb = fair_ > mid ? 0.65 : (fair_ < mid ? 0.35 : 0.5);
    const Side side = uniform() < buyProb ? Side::Buy : Side::Sell;

    OrderRequest req;
    req.side = side;
    req.qty = aggressiveQty();
    req.owner = kBotOwner;
    if (uniform() < 0.7) {
        req.type = OrderType::Market;
    } else {
        req.type = OrderType::IOC;
        req.price = side == Side::Buy ? top.bestAsk + 2 : std::max<Price>(1, top.bestBid - 2);
    }
    engine_.submit(req);
}

Qty MarketMaker::passiveQty() {
    std::lognormal_distribution<double> dist(2.3, 0.7);  // median ~10
    return std::clamp<Qty>(static_cast<Qty>(std::llround(dist(rng_))), 1, 500);
}

Qty MarketMaker::aggressiveQty() {
    std::lognormal_distribution<double> dist(2.0, 0.8);  // median ~7
    return std::clamp<Qty>(static_cast<Qty>(std::llround(dist(rng_))), 1, 300);
}

int MarketMaker::passiveOffset() {
    std::geometric_distribution<int> dist(0.18);
    return std::min(1 + dist(rng_), 30);
}

double MarketMaker::uniform() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

}  // namespace lob
