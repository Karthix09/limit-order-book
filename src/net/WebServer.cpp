#include "net/WebServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "net/Crypto.hpp"

namespace net {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr std::uint64_t kMaxFrameBytes = 1024 * 1024;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

void setTimeout(int fd, int option, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, option, &tv, sizeof tv);
}

const char* reasonPhrase(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string contentTypeFor(const std::string& path) {
    const auto dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : toLower(path.substr(dot + 1));
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json") return "application/json";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "ico") return "image/x-icon";
    return "application/octet-stream";
}

bool containsToken(const std::string& headerValue, const std::string& token) {
    std::stringstream ss(toLower(headerValue));
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (trim(part) == token) return true;
    }
    return false;
}

}  // namespace

WebServer::WebServer(std::string staticRoot) : staticRoot_(std::move(staticRoot)) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start(const std::string& bindAddress, std::uint16_t port, std::string& error) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind address: " + bindAddress;
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        error = "bind " + bindAddress + ":" + std::to_string(port) + ": " + std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 128) < 0) {
        error = std::string("listen: ") + std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&WebServer::acceptLoop, this);
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;

    if (acceptThread_.joinable()) acceptThread_.join();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }

    std::unique_lock lock(fdMutex_);
    // Wake every worker blocked in recv(); they close their own descriptors.
    for (int fd : openFds_) ::shutdown(fd, SHUT_RDWR);
    fdCv_.wait_for(lock, std::chrono::seconds(5), [&] { return activeWorkers_ == 0; });
}

void WebServer::acceptLoop() {
    while (running_) {
        pollfd pfd{};
        pfd.fd = listenFd_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 200);
        if (ready <= 0 || !running_) continue;

        sockaddr_in peer{};
        socklen_t peerLen = sizeof peer;
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) continue;

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        trackFd(fd);
        std::thread([this, fd] {
            handleConnection(fd);
            untrackFd(fd);
            ::close(fd);
            std::lock_guard lock(fdMutex_);
            --activeWorkers_;
            fdCv_.notify_all();
        }).detach();
    }
}

void WebServer::trackFd(int fd) {
    std::lock_guard lock(fdMutex_);
    openFds_.insert(fd);
    ++activeWorkers_;
}

void WebServer::untrackFd(int fd) {
    std::lock_guard lock(fdMutex_);
    openFds_.erase(fd);
}

void WebServer::handleConnection(int fd) {
    setTimeout(fd, SO_RCVTIMEO, 10);
    setTimeout(fd, SO_SNDTIMEO, 2);

    std::string buffer;
    HttpRequest request;
    if (!readHttpRequest(fd, buffer, request)) {
        HttpResponse bad;
        bad.status = 400;
        bad.contentType = "text/plain";
        bad.body = "bad request\n";
        writeResponse(fd, bad, false);
        return;
    }

    const bool upgrade = request.method == "GET" && request.path == "/ws" &&
                         containsToken(request.header("connection"), "upgrade") &&
                         toLower(request.header("upgrade")) == "websocket";
    if (!upgrade) {
        serveHttp(fd, request);
        return;
    }

    const std::string key = request.header("sec-websocket-key");
    if (key.empty() || request.header("sec-websocket-version") != "13") {
        HttpResponse bad;
        bad.status = 400;
        bad.contentType = "text/plain";
        bad.body = "invalid websocket handshake\n";
        bad.headers.emplace_back("Sec-WebSocket-Version", "13");
        writeResponse(fd, bad, false);
        return;
    }

    const std::string handshake =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + webSocketAcceptKey(key) + "\r\n\r\n";
    if (!sendAll(fd, handshake.data(), handshake.size())) return;

    setTimeout(fd, SO_RCVTIMEO, 0);  // block indefinitely; stop() wakes us via shutdown()

    auto conn = std::make_shared<Connection>();
    conn->id = nextClientId_++;
    conn->fd = fd;
    {
        std::lock_guard lock(connMutex_);
        connections_[conn->id] = conn;
    }
    if (onOpen_) onOpen_(conn->id);

    runWebSocket(conn, std::move(buffer));

    {
        std::lock_guard lock(connMutex_);
        connections_.erase(conn->id);
    }
    {
        // After this no broadcaster can write to the (soon to be closed) fd.
        std::lock_guard lock(conn->writeMutex);
        conn->open = false;
    }
    if (onClose_) onClose_(conn->id);
}

bool WebServer::readHttpRequest(int fd, std::string& buffer, HttpRequest& request) {
    char chunk[4096];
    std::size_t headerEnd = std::string::npos;
    while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return false;
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }

    std::istringstream head(buffer.substr(0, headerEnd));
    std::string line;
    if (!std::getline(head, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream rl(line);
        std::string target;
        if (!(rl >> request.method >> target >> request.version)) return false;
        const auto q = target.find('?');
        request.path = target.substr(0, q);
        request.query = q == std::string::npos ? "" : target.substr(q + 1);
    }
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        request.headers[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }

    std::size_t contentLength = 0;
    const std::string cl = request.header("content-length");
    if (!cl.empty()) {
        try {
            contentLength = static_cast<std::size_t>(std::stoull(cl));
        } catch (...) {
            return false;
        }
        if (contentLength > kMaxBodyBytes) return false;
    }

    const std::size_t bodyStart = headerEnd + 4;
    while (buffer.size() < bodyStart + contentLength) {
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
    request.body = buffer.substr(bodyStart, contentLength);
    buffer.erase(0, bodyStart + contentLength);  // keep any bytes that followed the request
    return true;
}

void WebServer::serveHttp(int fd, const HttpRequest& request) {
    if (request.method == "OPTIONS") {
        HttpResponse r;
        r.status = 204;
        r.contentType = "text/plain";
        r.headers.emplace_back("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.headers.emplace_back("Access-Control-Allow-Headers", "Content-Type");
        writeResponse(fd, r, false);
        return;
    }

    if (onHttp_) {
        if (auto response = onHttp_(request)) {
            writeResponse(fd, *response, request.method == "HEAD");
            return;
        }
    }

    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse r;
        r.status = 405;
        r.contentType = "text/plain";
        r.body = "method not allowed\n";
        writeResponse(fd, r, false);
        return;
    }
    writeResponse(fd, serveStatic(request), request.method == "HEAD");
}

HttpResponse WebServer::serveStatic(const HttpRequest& request) const {
    HttpResponse r;
    std::string path = request.path == "/" ? "/index.html" : request.path;
    if (path.find("..") != std::string::npos || path.find('\0') != std::string::npos) {
        r.status = 403;
        r.contentType = "text/plain";
        r.body = "forbidden\n";
        return r;
    }
    std::ifstream file(staticRoot_ + path, std::ios::binary);
    if (!file) {
        r.status = 404;
        r.contentType = "text/plain";
        r.body = "not found: " + path + "\n";
        return r;
    }
    std::ostringstream content;
    content << file.rdbuf();
    r.body = content.str();
    r.contentType = contentTypeFor(path);
    return r;
}

void WebServer::writeResponse(int fd, const HttpResponse& response, bool headOnly) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + reasonPhrase(response.status) + "\r\n";
    out += "Content-Type: " + response.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Access-Control-Allow-Origin: *\r\n";
    for (const auto& [name, value] : response.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
    if (!headOnly) out += response.body;
    sendAll(fd, out.data(), out.size());
}

void WebServer::runWebSocket(const std::shared_ptr<Connection>& conn, std::string buffer) {
    std::string message;
    bool inMessage = false;
    char chunk[8192];

    while (true) {
        // Process every complete frame currently buffered.
        while (true) {
            if (buffer.size() < 2) break;
            const auto* b = reinterpret_cast<const unsigned char*>(buffer.data());
            const bool fin = (b[0] & 0x80) != 0;
            const std::uint8_t opcode = b[0] & 0x0F;
            const bool masked = (b[1] & 0x80) != 0;
            std::uint64_t len = b[1] & 0x7F;
            std::size_t pos = 2;

            if (len == 126) {
                if (buffer.size() < 4) break;
                len = (std::uint64_t{b[2]} << 8) | b[3];
                pos = 4;
            } else if (len == 127) {
                if (buffer.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | b[2 + i];
                pos = 10;
            }
            if (!masked || len > kMaxFrameBytes) {
                // RFC 6455: client frames must be masked. 1002 = protocol error, 1009 = too big.
                const std::string code = !masked ? std::string("\x03\xEA", 2) : std::string("\x03\xF1", 2);
                sendRaw(*conn, encodeFrame(0x8, code));
                return;
            }
            if (buffer.size() < pos + 4 + len) break;

            unsigned char mask[4];
            std::memcpy(mask, b + pos, 4);
            pos += 4;
            std::string payload = buffer.substr(pos, static_cast<std::size_t>(len));
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            buffer.erase(0, pos + static_cast<std::size_t>(len));

            switch (opcode) {
                case 0x0:  // continuation
                    if (!inMessage) return;
                    message += payload;
                    if (message.size() > kMaxFrameBytes) return;
                    if (fin) {
                        inMessage = false;
                        if (onMessage_) onMessage_(conn->id, message);
                        message.clear();
                    }
                    break;
                case 0x1:  // text
                case 0x2:  // binary (treated as text payload)
                    if (fin) {
                        if (onMessage_) onMessage_(conn->id, payload);
                    } else {
                        inMessage = true;
                        message = std::move(payload);
                    }
                    break;
                case 0x8:  // close: echo status code and finish
                    sendRaw(*conn, encodeFrame(0x8, payload.substr(0, 2)));
                    return;
                case 0x9:  // ping
                    sendRaw(*conn, encodeFrame(0xA, payload));
                    break;
                case 0xA:  // pong
                    break;
                default:
                    return;
            }
        }

        const ssize_t n = ::recv(conn->fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
}

std::string WebServer::encodeFrame(std::uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(0x80 | opcode));
    const std::uint64_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
    frame += payload;
    return frame;
}

bool WebServer::sendAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, kSendFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool WebServer::sendRaw(Connection& conn, const std::string& frame) {
    std::lock_guard lock(conn.writeMutex);
    if (!conn.open) return false;
    if (!sendAll(conn.fd, frame.data(), frame.size())) {
        // Slow or dead peer: drop it. shutdown() wakes its reader thread, which cleans up.
        conn.open = false;
        ::shutdown(conn.fd, SHUT_RDWR);
        return false;
    }
    return true;
}

bool WebServer::send(ClientId client, const std::string& text) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard lock(connMutex_);
        auto it = connections_.find(client);
        if (it == connections_.end()) return false;
        conn = it->second;
    }
    return sendRaw(*conn, encodeFrame(0x1, text));
}

void WebServer::broadcast(const std::string& text) {
    std::vector<std::shared_ptr<Connection>> targets;
    {
        std::lock_guard lock(connMutex_);
        targets.reserve(connections_.size());
        for (const auto& [id, conn] : connections_) targets.push_back(conn);
    }
    if (targets.empty()) return;
    const std::string frame = encodeFrame(0x1, text);
    for (const auto& conn : targets) sendRaw(*conn, frame);
}

std::size_t WebServer::clientCount() const {
    std::lock_guard lock(connMutex_);
    return connections_.size();
}

}  // namespace net
