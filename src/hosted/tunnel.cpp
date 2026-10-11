#include <winsock2.h>

#include "app/log.h"
#include "config/config.h"
#include "hosted/tunnel.h"
#include <array>
#include <utility>

namespace tmloader::hosted
{
namespace
{

constexpr int kChunkBytes = 65536;

// The client's hardcoded Game IRC port is redirected to the game tunnel, so the
// profile listener must not land on it.
auto listenProfile(unsigned short& port) -> wil::unique_socket
{
    auto listener = listenLoopback(port);
    if (port == kClientGamePort)
    {
        auto other = listenLoopback(port);
        return other;
    }

    return listener;
}

auto sendAll(SOCKET native, std::span<const char> data) -> bool
{
    while (!data.empty())
    {
        const auto sent = send(native, data.data(), static_cast<int>(data.size()), 0);
        if (sent <= 0)
        {
            return false;
        }

        data = data.subspan(static_cast<size_t>(sent));
    }

    return true;
}

} // namespace

auto listenLoopback(unsigned short& port) -> wil::unique_socket
{
    auto listener           = wil::unique_socket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    auto address            = sockaddr_in{ AF_INET };
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto length             = int{ sizeof(address) };
    if (!listener || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener.get(), SOMAXCONN) ||
        getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length))
    {
        throw std::runtime_error("Cannot open a local listener");
    }

    port = ntohs(address.sin_port);
    return listener;
}

Tunnel::Tunnel(HttpClient& http, Streams streams)
: http_(http)
, streams_(std::move(streams))
, gameListener_(listenLoopback(gamePort_))
, profileListener_(listenProfile(profilePort_))
, acceptor_([this](const std::stop_token& stop)
            {
                acceptLoop(stop);
            })
{
}

Tunnel::~Tunnel()
{
    acceptor_.request_stop();
    acceptor_.join();
    closeConnections();
    workers_.clear(); // Joins every forwarding thread.
}

auto Tunnel::gamePort() const noexcept -> unsigned short
{
    return gamePort_;
}

auto Tunnel::profilePort() const noexcept -> unsigned short
{
    return profilePort_;
}

void Tunnel::closeConnections() noexcept
{
    const std::lock_guard lock(connectionsMutex_);
    for (const auto native : connections_)
    {
        shutdown(native, SD_BOTH);
    }
}

void Tunnel::acceptLoop(const std::stop_token& stop)
{
    while (!stop.stop_requested())
    {
        auto ready = fd_set{};
        FD_SET(gameListener_.get(), &ready);
        FD_SET(profileListener_.get(), &ready);
        const auto wait = timeval{ 0, 250000 };
        if (select(0, &ready, nullptr, nullptr, &wait) <= 0)
        {
            continue;
        }

        for (const auto& [listener, url] : { std::pair{ gameListener_.get(), &streams_.game }, std::pair{ profileListener_.get(), &streams_.profile } })
        {
            if (!FD_ISSET(listener, &ready))
            {
                continue;
            }

            const auto native = accept(listener, nullptr, nullptr);
            if (native == INVALID_SOCKET)
            {
                continue;
            }

            {
                const std::lock_guard lock(connectionsMutex_);
                connections_.push_back(native);
            }

            std::erase_if(workers_, [](const Worker& worker)
                          {
                              return worker.done->load();
                          });
            auto done = std::make_shared<std::atomic<bool>>(false);
            workers_.push_back({ std::jthread([this, native, url, done]
                                              {
                                                  forward(native, *url);
                                                  *done = true;
                                              }),
                                 done });
        }
    }
}

void Tunnel::forward(SOCKET native, const std::string& url)
{
    try
    {
        auto socket     = http_.openWebSocket(url, streams_.token, streams_.subprotocol);
        auto downstream = std::jthread([&]
                                       {
                                           try
                                           {
                                               while (const auto message = socket.receive())
                                               {
                                                   if (!sendAll(native, *message))
                                                   {
                                                       break;
                                                   }
                                               }
                                           }
                                           catch (const std::exception& error)
                                           {
                                               logLine("Stream receive ended: {}", error.what());
                                           }

                                           // Remote closure becomes EOF for the game.
                                           shutdown(native, SD_BOTH);
                                       });
        auto buffer     = std::array<char, kChunkBytes>{};
        try
        {
            while (true)
            {
                const auto read = recv(native, buffer.data(), kChunkBytes, 0);
                if (read <= 0)
                {
                    break;
                }

                socket.send({ buffer.data(), static_cast<size_t>(read) });
            }
        }
        catch (const std::exception& error)
        {
            logLine("Stream send ended: {}", error.what());
        }

        // Closing also ends the pending receive on the other thread.
        socket.close(1000);
        downstream.join();
        logLine("Stream closed with status {}", socket.closeStatus());
    }
    catch (const std::exception& error)
    {
        logLine("Stream failed: {}", error.what());
    }

    const std::lock_guard lock(connectionsMutex_);
    std::erase(connections_, native);
    closesocket(native);
}

} // namespace tmloader::hosted
