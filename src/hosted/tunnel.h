#pragma once
#include "hosted/http.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tmloader::hosted
{

// Listens on an ephemeral 127.0.0.1 port and reports it through port.
auto listenLoopback(unsigned short& port) -> wil::unique_socket;

// Loopback listeners for the game's two services. Each accepted native
// connection is forwarded byte for byte over its own authenticated WebSocket.
class Tunnel
{
public:
    struct Streams
    {
        std::string game;
        std::string profile;
        std::string token;
        std::string subprotocol;
    };

    Tunnel(HttpClient& http, Streams streams);
    ~Tunnel();
    Tunnel(const Tunnel&)                    = delete;
    auto operator=(const Tunnel&) -> Tunnel& = delete;
    Tunnel(Tunnel&&)                         = delete;
    auto operator=(Tunnel&&) -> Tunnel&      = delete;

    auto gamePort() const noexcept -> unsigned short;
    auto profilePort() const noexcept -> unsigned short;
    // Ends every forwarded connection; the game sees them close.
    void closeConnections() noexcept;

private:
    void acceptLoop(const std::stop_token& stop);
    void forward(SOCKET native, const std::string& url);

    HttpClient& http_;
    Streams     streams_;
    // Ports precede the listeners that fill them in during construction.
    unsigned short      gamePort_    = 0;
    unsigned short      profilePort_ = 0;
    wil::unique_socket  gameListener_;
    wil::unique_socket  profileListener_;
    std::mutex          connectionsMutex_;
    std::vector<SOCKET> connections_;

    struct Worker
    {
        std::jthread                       thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    std::vector<Worker> workers_;
    std::jthread        acceptor_;
};

} // namespace tmloader::hosted
