
// Serving Observatory — RAII TCP sockets (Winsock, Windows-first).
// Copyright 2026 Summon Software Labs. Apache-2.0.

#include "servingobs/store/network.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#include <utility>

namespace servingobs {

namespace {
void ensure_wsa() { wsa_startup(); }
}

bool wsa_startup() {
    static bool init = []() -> bool {
        WSADATA d;
        // On modern Windows, winsock is always usable; ignore minor failures
        // but surface the error string to callers that need it.
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return init;
}

void wsa_cleanup() { WSACleanup(); }

string last_wsa_error(int err) {
    if (err == 0) err = WSAGetLastError();
    std::ostringstream os;
    os << "winsock error " << err;
    return os.str();
}

// ---------------------------------------------------------------- stream
TcpStream::~TcpStream() { close(); }
TcpStream::TcpStream(TcpStream&& o) noexcept : sock_(o.sock_) { o.sock_ = INVALID_SOCKET; }
TcpStream& TcpStream::operator=(TcpStream&& o) noexcept {
    if (this != &o) { close(); sock_ = o.sock_; o.sock_ = INVALID_SOCKET; }
    return *this;
}
void TcpStream::close() {
    if (sock_ != INVALID_SOCKET) {
        shutdown(sock_, SD_BOTH);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}
void TcpStream::shutdown_send() { if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_SEND); }
void TcpStream::set_timeout(int ms) {
    if (sock_ == INVALID_SOCKET) return;
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

Result<TcpStream> TcpStream::connect(const string& host, u16 port, int timeout_ms) {
    ensure_wsa();
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    string pstr = std::to_string(port);
    if (getaddrinfo(host.c_str(), pstr.c_str(), &hints, &res) != 0) {
        return Result<TcpStream>::Err("getaddrinfo failed: " + last_wsa_error());
    }
    SOCKET s = INVALID_SOCKET;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        return Result<TcpStream>::Err("connect failed: " + last_wsa_error());
    }
    TcpStream t = TcpStream::from_socket(s);
    t.set_timeout(timeout_ms);
    return Result<TcpStream>::Ok(std::move(t));
}

Result<bool> TcpStream::send_all(const byte* data, std::size_t n) {
    if (sock_ == INVALID_SOCKET) return Result<bool>::Err("send on closed socket");
    std::lock_guard<std::mutex> lk(write_mu_);
    const char* p = reinterpret_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < n) {
        int chunk = static_cast<int>(std::min<std::size_t>(n - sent, INT_MAX));
        int w = send(sock_, p + sent, chunk, 0);
        if (w == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT) continue;
            return Result<bool>::Err("send failed: " + last_wsa_error());
        }
        if (w == 0) return Result<bool>::Err("send reached zero bytes");
        sent += static_cast<std::size_t>(w);
    }
    return Result<bool>::Ok(true);
}

Result<bytes> TcpStream::recv_exact(std::size_t n) {
    if (sock_ == INVALID_SOCKET) return Result<bytes>::Err("recv on closed socket");
    bytes out;
    out.reserve(n);
    char tmp[65536];
    while (out.size() < n) {
        std::size_t want = std::min<std::size_t>(n - out.size(), sizeof(tmp));
        int r = recv(sock_, tmp, static_cast<int>(want), 0);
        if (r == 0) return Result<bytes>::Err("peer closed during recv");
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAETIMEDOUT) return Result<bytes>::Err("recv timeout");
            return Result<bytes>::Err("recv failed: " + last_wsa_error(e));
        }
        out.insert(out.end(), reinterpret_cast<byte*>(tmp), reinterpret_cast<byte*>(tmp + r));
    }
    return Result<bytes>::Ok(std::move(out));
}

Result<std::optional<bytes>> TcpStream::recv_frame() {
    auto lenres = recv_exact(4);
    if (!lenres.ok()) {
        // Distinguish clean close from errors: a 0-byte read returns Err with
        // "peer closed" — treat transient timeout as empty too.
        return Result<std::optional<bytes>>::Ok(std::nullopt);
    }
    const bytes& hdr = lenres.value();
    u32 len = 0;
    for (int i = 0; i < 4; ++i) len = (len << 8) | static_cast<u32>(hdr[i]);
    if (len < 8 || len > (256u * 1024u * 1024u)) {
        return Result<std::optional<bytes>>::Err("invalid frame length " + std::to_string(len));
    }
    auto body = recv_exact(len);
    if (!body.ok()) return Result<std::optional<bytes>>::Err("frame body read failed: " + body.error());
    bytes full;
    full.reserve(4 + len);
    full.insert(full.end(), hdr.begin(), hdr.end());
    full.insert(full.end(), body.value().begin(), body.value().end());
    return Result<std::optional<bytes>>::Ok(std::move(full));
}

Result<std::optional<bytes>> TcpStream::recv_some(std::size_t max) {
    if (sock_ == INVALID_SOCKET) return Result<std::optional<bytes>>::Ok(std::nullopt);
    char tmp[65536];
    std::size_t want = std::min<std::size_t>(max, sizeof(tmp));
    int r = recv(sock_, tmp, static_cast<int>(want), 0);
    if (r == 0) return Result<std::optional<bytes>>::Ok(std::nullopt);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAETIMEDOUT) return Result<std::optional<bytes>>::Ok(std::nullopt);
        return Result<std::optional<bytes>>::Err("recv failed: " + last_wsa_error(e));
    }
    bytes out;
    out.insert(out.end(), reinterpret_cast<byte*>(tmp), reinterpret_cast<byte*>(tmp + r));
    return Result<std::optional<bytes>>::Ok(std::move(out));
}

// ---------------------------------------------------------------- listener
TcpListener::~TcpListener() { close(); }
TcpListener::TcpListener(TcpListener&& o) noexcept : sock_(o.sock_), port_(o.port_) { o.sock_ = INVALID_SOCKET; o.port_ = 0; }
TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
    if (this != &o) { close(); sock_ = o.sock_; port_ = o.port_; o.sock_ = INVALID_SOCKET; o.port_ = 0; }
    return *this;
}
void TcpListener::close() {
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
}
void TcpListener::listen_backlog(int backlog) {
    if (sock_ != INVALID_SOCKET) ::listen(sock_, backlog);
}

Result<TcpListener> TcpListener::bind(const string& host, u16 port) {
    ensure_wsa();
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    string pstr = std::to_string(port);
    if (getaddrinfo(host.empty() ? nullptr : host.c_str(), pstr.c_str(), &hints, &res) != 0) {
        return Result<TcpListener>::Err("getaddrinfo failed: " + last_wsa_error());
    }
    SOCKET s = INVALID_SOCKET;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return Result<TcpListener>::Err("bind failed: " + last_wsa_error());
    if (::listen(s, 64) == SOCKET_ERROR) {
        closesocket(s);
        return Result<TcpListener>::Err("listen failed: " + last_wsa_error());
    }
    TcpListener l;
    l.sock_ = s;
    // Determine actual port (0 = ephemeral).
    sockaddr_in addr; int alen = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) == 0) {
        u16 p = ntohs(addr.sin_port);
        l.port_ = p;
    }
    return Result<TcpListener>::Ok(std::move(l));
}

Result<TcpStream> TcpListener::accept(int timeout_ms) {
    if (sock_ == INVALID_SOCKET) return Result<TcpStream>::Err("accept on closed listener");
    // Set a receive timeout on the listening socket so accept() yields.
    if (timeout_ms >= 0) {
        DWORD t = static_cast<DWORD>(timeout_ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    }
    SOCKET c = ::accept(sock_, nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAETIMEDOUT)
            return Result<TcpStream>::Err("accept timeout");
        return Result<TcpStream>::Err("accept failed: " + last_wsa_error(e));
    }
    return Result<TcpStream>::Ok(TcpStream::from_socket(c));
}

} // namespace servingobs
