#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "app/Gateway.hpp"
#include "lob/MarketMaker.hpp"
#include "lob/MatchingEngine.hpp"
#include "net/WebServer.hpp"

#ifndef LOB_WEB_ROOT
#define LOB_WEB_ROOT "web"
#endif

namespace {

volatile std::sig_atomic_t gStop = 0;

void onSignal(int) { gStop = 1; }

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "  --port N          HTTP/WebSocket port (default 8080)\n"
              << "  --bind ADDR       bind address (default 127.0.0.1; use 0.0.0.0 for LAN)\n"
              << "  --web DIR         directory with index.html (default " << LOB_WEB_ROOT << ")\n"
              << "  --rate N          bot actions per second, 1-2000 (default 40)\n"
              << "  --mid PRICE       initial mid price (default 100.00)\n"
              << "  --depth N         price levels per side sent to clients (default 25)\n"
              << "  --publish-ms N    publish interval in milliseconds (default 100)\n"
              << "  --paused          start with the market-maker bot paused\n"
              << "  --seed N          RNG seed for reproducible bot flow\n"
              << "  -h, --help        show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string bindAddress = "127.0.0.1";
    std::string webRoot = LOB_WEB_ROOT;
    int port = 8080;
    lob::MarketMakerConfig botConfig;
    app::GatewayConfig gatewayConfig;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        try {
            if (arg == "--port") port = std::stoi(next());
            else if (arg == "--bind") bindAddress = next();
            else if (arg == "--web") webRoot = next();
            else if (arg == "--rate") botConfig.actionsPerSecond = std::stoi(next());
            else if (arg == "--mid") botConfig.initialMid = static_cast<lob::Price>(std::stod(next()) * lob::kTicksPerUnit + 0.5);
            else if (arg == "--depth") gatewayConfig.bookDepth = static_cast<std::size_t>(std::stoul(next()));
            else if (arg == "--publish-ms") gatewayConfig.publishInterval = std::chrono::milliseconds(std::stoi(next()));
            else if (arg == "--paused") botConfig.startRunning = false;
            else if (arg == "--seed") botConfig.seed = std::stoull(next());
            else if (arg == "-h" || arg == "--help") {
                printUsage(argv[0]);
                return 0;
            } else {
                std::cerr << "unknown option: " << arg << "\n";
                printUsage(argv[0]);
                return 2;
            }
        } catch (const std::exception&) {
            std::cerr << "invalid value for " << arg << "\n";
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "port out of range\n";
        return 2;
    }
    if (botConfig.initialMid < 2 * lob::kTicksPerUnit) {
        std::cerr << "--mid must be at least 2.00\n";
        return 2;
    }
    if (gatewayConfig.publishInterval.count() < 10) gatewayConfig.publishInterval = std::chrono::milliseconds(10);
    if (gatewayConfig.bookDepth == 0) gatewayConfig.bookDepth = 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    lob::MatchingEngine engine;
    lob::MarketMaker bot(engine, botConfig);
    net::WebServer server(webRoot);
    app::Gateway gateway(engine, bot, server, gatewayConfig);

    gateway.attach();
    std::string error;
    if (!server.start(bindAddress, static_cast<std::uint16_t>(port), error)) {
        std::cerr << "failed to start server: " << error << "\n";
        return 1;
    }
    bot.start();
    gateway.start();

    std::cout << "LOB simulator running\n"
              << "  UI:        http://" << (bindAddress == "0.0.0.0" ? "localhost" : bindAddress) << ":" << port << "/\n"
              << "  WebSocket: ws://" << (bindAddress == "0.0.0.0" ? "localhost" : bindAddress) << ":" << port << "/ws\n"
              << "  Static:    " << webRoot << "\n"
              << "  Bot:       " << (botConfig.startRunning ? "running" : "paused") << " @ " << bot.rate()
              << " actions/s\n"
              << "Press Ctrl+C to stop." << std::endl;

    while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\nshutting down...\n";
    bot.stop();
    gateway.stop();
    server.stop();
    return 0;
}
