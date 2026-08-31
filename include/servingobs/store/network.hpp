
#pragma once
// Serving Observatory — RAII TCP sockets (Winsock, Windows-first).
// Copyright 2026 Summon Software Labs. Apache-2.0.
//
// Safe socket ownership: handles wrap in RAII, shutdown/close in destructors.
// Sends on a connection are serialized by a per-connection mutex so concurrent
// writers cannot interleave frames. Receiving enforces exact-length reads.

#include "servingobs/core/types.hpp"
#include "servingobs/core/identity.hpp"

#include <mutex>
#include <string>

// Winsock always before windows.h (which may be pulled in elsewhere).
#include <winsock2.h>
#include <ws2tcpip.h>

namespace servingobs {

bool wsa_startup();
void wsa_cleanup();

// A connected TCP socket.
class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& o) noexcept;
    TcpStream& operator=(TcpStream&& o) noexcept;

    bool valid() const { return sock_ != INVALID_SOCKET; }
    void close();

    static Result<TcpStream> connect(const string& host, u16 port, int timeout_ms = 10000);
    static TcpStream from_socket(SOCKET s) { TcpStream t; t.sock_ = s; return t; }

    // Send exactly n bytes. Serialized by an internal mutex.
    Result<bool> send_all(const byte* data, std::size_t n);
    Result<bool> send_all(const bytes& b) { return send_all(b.data(), b.size()); }

    // Receive exactly n bytes or fail.
    Result<bytes> recv_exact(std::size_t n);

    // Receive one complete frame (len prefix + body) or fail / close.
    Result<std::optional<bytes>> recv_frame();
    // Receive up to n bytes (any amount), returns empty optional on peer close.
    Result<std::optional<bytes>> recv_some(std::size_t max);

    void set_timeout(int ms);
    // Force a graceful shutdown of the send side.
    void shutdown_send();

private:
    SOCKET sock_ = INVALID_SOCKET;
    std::mutex write_mu_;
};

// A bound TCP listener.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& o) noexcept;
    TcpListener& operator=(TcpListener&& o) noexcept;

    static Result<TcpListener> bind(const string& host, u16 port);
    void close();
    u16 port() const { return port_; }

    // Accept with an optional timeout (ms). Returns an invalid stream on timeout.
    Result<TcpStream> accept(int timeout_ms = -1);
    void listen_backlog(int backlog = 64);

private:
    SOCKET sock_ = INVALID_SOCKET;
    u16 port_ = 0;
};

string last_wsa_error(int err = 0);

} // namespace servingobs
