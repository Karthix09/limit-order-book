#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace net {

using ClientId = std::uint64_t;

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;  // lower-cased names
    std::string body;

    std::string header(const std::string& lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string{} : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Dependency-free HTTP/1.1 + WebSocket (RFC 6455) server on POSIX sockets.
//   * One thread per connection (fine for a local simulator with a few viewers).
//   * GET /ws upgrades to a WebSocket; text frames go to the message handler.
//   * Other requests go to the HTTP handler first; if it declines (nullopt),
//     GET/HEAD requests are served from the static root directory.
class WebServer {
public:
    using OpenHandler = std::function<void(ClientId)>;
    using CloseHandler = std::function<void(ClientId)>;
    using MessageHandler = std::function<void(ClientId, const std::string&)>;
    using HttpHandler = std::function<std::optional<HttpResponse>(const HttpRequest&)>;

    explicit WebServer(std::string staticRoot);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    void setOpenHandler(OpenHandler h) { onOpen_ = std::move(h); }
    void setCloseHandler(CloseHandler h) { onClose_ = std::move(h); }
    void setMessageHandler(MessageHandler h) { onMessage_ = std::move(h); }
    void setHttpHandler(HttpHandler h) { onHttp_ = std::move(h); }

    // Handlers must be set before start().
    bool start(const std::string& bindAddress, std::uint16_t port, std::string& error);
    void stop();

    bool send(ClientId client, const std::string& text);
    void broadcast(const std::string& text);
    std::size_t clientCount() const;

private:
    struct Connection {
        ClientId id = 0;
        int fd = -1;
        std::mutex writeMutex;
        bool open = true;  // guarded by writeMutex
    };

    void acceptLoop();
    void handleConnection(int fd);
    bool readHttpRequest(int fd, std::string& buffer, HttpRequest& request);
    void serveHttp(int fd, const HttpRequest& request);
    HttpResponse serveStatic(const HttpRequest& request) const;
    void runWebSocket(const std::shared_ptr<Connection>& conn, std::string buffer);
    bool sendRaw(Connection& conn, const std::string& frame);

    static std::string encodeFrame(std::uint8_t opcode, const std::string& payload);
    static bool sendAll(int fd, const char* data, std::size_t len);
    static void writeResponse(int fd, const HttpResponse& response, bool headOnly);

    void trackFd(int fd);
    void untrackFd(int fd);

    std::string staticRoot_;
    OpenHandler onOpen_;
    CloseHandler onClose_;
    MessageHandler onMessage_;
    HttpHandler onHttp_;

    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::atomic<ClientId> nextClientId_{1};

    mutable std::mutex connMutex_;
    std::unordered_map<ClientId, std::shared_ptr<Connection>> connections_;

    std::mutex fdMutex_;
    std::condition_variable fdCv_;
    std::unordered_set<int> openFds_;
    int activeWorkers_ = 0;  // guarded by fdMutex_
};

}  // namespace net
