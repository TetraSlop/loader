// ws2_32.dll on the host's stream sockets (host/host.h): what polcore uses on TM's paths (browser/docs/polcore.md,
// "Sockets on tmserver's browser streams"). polcore opens non-blocking TCP sockets, starts connect, and polls them
// with select and a zero timeout, then send, recv and getsockopt(SO_ERROR). Each socket is one host stream, which
// the host routes by port (the game or a profile stream). A blocking call parks the thread until the stream changes.
#include "hle/process.h"
#include "host/host.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <string_view>
#include <vector>

namespace tmweb::hle
{
namespace
{

enum WinsockError : std::uint32_t
{
    kWsaEWouldBlock    = 10035,
    kWsaEAlready       = 10037,
    kWsaENotSock       = 10038,
    kWsaEAfNoSupport   = 10047,
    kWsaEConnReset     = 10054,
    kWsaEIsConn        = 10056,
    kWsaENotConn       = 10057,
    kWsaEConnRefused   = 10061,
    kWsaHostNotFound   = 11001,
    kWsaNotInitialised = 10093,
};

constexpr std::uint32_t kSocketError   = 0xFFFFFFFF;
constexpr std::uint32_t kInvalidSocket = 0xFFFFFFFF;
constexpr std::uint32_t kFionbio       = 0x8004667E;
constexpr std::uint32_t kFionread      = 0x4004667F;
constexpr std::uint32_t kSolSocket     = 0xFFFF;
constexpr std::uint32_t kSoError       = 0x1007;
constexpr std::uint16_t kInet          = 2;
constexpr unsigned      kFdSetSize     = 64;
// ws2_32.dll's export ordinals: polcore imports most of these by ordinal
const auto kOrdinals = std::map<std::string_view, unsigned>{
    { "accept", 1 },
    { "bind", 2 },
    { "closesocket", 3 },
    { "connect", 4 },
    { "getpeername", 5 },
    { "getsockname", 6 },
    { "getsockopt", 7 },
    { "htonl", 8 },
    { "htons", 9 },
    { "ioctlsocket", 10 },
    { "listen", 13 },
    { "ntohl", 14 },
    { "ntohs", 15 },
    { "recv", 16 },
    { "recvfrom", 17 },
    { "select", 18 },
    { "send", 19 },
    { "sendto", 20 },
    { "setsockopt", 21 },
    { "shutdown", 22 },
    { "socket", 23 },
    { "gethostbyname", 52 },
    { "WSAGetLastError", 111 },
    { "WSASetLastError", 112 },
    { "WSAStartup", 115 },
    { "WSACleanup", 116 },
    { "__WSAFDIsSet", 151 },
};
// gethostbyname's answer for every name: the host routes streams by port, so the address only has to be IPv4
constexpr std::array<std::uint8_t, 4> kHostAddress{ 127, 0, 0, 1 };

struct Socket final : Object
{
    std::shared_ptr<host::Socket> stream;
    bool                          nonBlocking = false;
    bool                          wasOpen     = false; // connect completed, so a later failure is a reset

    auto state() -> host::SocketState
    {
        const auto now = stream->state();
        wasOpen        = wasOpen || now == host::SocketState::Open;
        return now;
    }

    auto readable() -> bool
    {
        if (!stream)
        {
            return false;
        }
        const auto now = state();
        return stream->available() > 0 || now == host::SocketState::Closed || (now == host::SocketState::Failed && wasOpen);
    }

    auto writable() -> bool
    {
        return stream && state() == host::SocketState::Open;
    }

    // A connect that failed reports in exceptfds, as Windows does
    auto failed() -> bool
    {
        return stream && state() == host::SocketState::Failed && !wasOpen;
    }

    auto settled() -> bool
    {
        return !stream || state() != host::SocketState::Connecting;
    }
};

// A wait that ends when test() holds: the host's socket events arrive between run slices
struct Ready final : Waitable
{
    std::function<bool()> test;

    auto signaled(ThreadId) const -> bool override
    {
        return test();
    }
};

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

auto swap16(const std::uint32_t value) -> std::uint32_t
{
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

auto swap32(const std::uint32_t value) -> std::uint32_t
{
    return (value >> 24) | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | (value << 24);
}

} // namespace

void installWs2_32(Process& p)
{
    auto& boundary = p.machine.boundary();
    auto& memory   = p.machine.memory();
    // Each under its name and, as polcore imports them, its ordinal
    const auto add = [&](const char* name, const std::uint8_t argc, Handler handler)
    {
        if (const auto ordinal = kOrdinals.find(name); ordinal != kOrdinals.end())
        {
            boundary.add(Function{ "ws2_32.dll", std::format("#{}", ordinal->second), Convention::Stdcall, argc, handler });
        }
        boundary.add(Function{ "ws2_32.dll", name, Convention::Stdcall, argc, std::move(handler) });
    };
    const auto fail = [&p](const std::uint32_t error) -> Outcome
    {
        p.setLastError(error);
        return ret(kSocketError);
    };
    const auto socketOf = [&p](const std::uint32_t handle)
    {
        return p.handleAs<Socket>(handle);
    };
    const auto readSet = [&memory](const std::uint32_t set)
    {
        auto sockets = std::vector<std::uint32_t>{};
        if (set)
        {
            const auto count = std::min(memory.read<std::uint32_t>(set), kFdSetSize);
            for (auto i = 0u; i < count; ++i)
            {
                sockets.push_back(memory.read<std::uint32_t>(set + 4 + 4 * i));
            }
        }
        return sockets;
    };

    add("WSAStartup", 2, [&memory](Call& call)
        {
            // WSADATA: wVersion, wHighVersion, then descriptions; zeroed past the versions
            memory.fill(call.arg(1), 0, 400);
            memory.write<std::uint16_t>(call.arg(1), 0x0202);
            memory.write<std::uint16_t>(call.arg(1) + 2, 0x0202);
            return ret(0);
        });
    add("WSACleanup", 0, [](Call&)
        {
            return ret(0);
        });
    add("WSAGetLastError", 0, [&p, &memory](Call&)
        {
            return ret(memory.read<std::uint32_t>(p.thread().teb + 0x34));
        });
    add("WSASetLastError", 1, [&p](Call& call)
        {
            p.setLastError(call.arg(0));
            return ret();
        });
    add("htons", 1, [](Call& call)
        {
            return ret(swap16(call.arg(0)));
        });
    add("ntohs", 1, [](Call& call)
        {
            return ret(swap16(call.arg(0)));
        });
    add("htonl", 1, [](Call& call)
        {
            return ret(swap32(call.arg(0)));
        });
    add("ntohl", 1, [](Call& call)
        {
            return ret(swap32(call.arg(0)));
        });

    // One hostent for every name: { h_name, h_aliases, h_addrtype, h_length, h_addr_list }, the lists after it
    add("gethostbyname", 1, [&p, &memory](Call& call)
        {
            const auto name = call.argString(0);
            p.trace.push_back(std::format("gethostbyname {}", name));
            if (name.empty())
            {
                p.setLastError(kWsaHostNotFound);
                return ret(0);
            }
            const auto entry   = p.allocate(16 + 4 + 8 + 4 + static_cast<std::uint32_t>(name.size()) + 1);
            const auto aliases = entry + 16, list = entry + 20, address = entry + 28, text = entry + 32;
            memory.write<std::uint32_t>(entry, text);
            memory.write<std::uint32_t>(entry + 4, aliases);
            memory.write<std::uint16_t>(entry + 8, kInet);
            memory.write<std::uint16_t>(entry + 10, 4);
            memory.write<std::uint32_t>(entry + 12, list);
            memory.write<std::uint32_t>(list, address);
            memory.write(address, kHostAddress);
            memory.writeString(text, name);
            return ret(entry);
        });

    add("socket", 3, [&p, fail](Call& call)
        {
            // TCP only: AF_INET and SOCK_STREAM
            if (call.arg(0) != kInet || call.arg(1) != 1)
            {
                fail(kWsaEAfNoSupport);
                return ret(kInvalidSocket);
            }
            return ret(p.addHandle(std::make_shared<Socket>()));
        });
    add("bind", 3, [](Call&)
        {
            return ret(0); // polcore binds to any local address and port 0
        });
    add("setsockopt", 5, [](Call&)
        {
            return ret(0);
        });
    add("getsockopt", 5, [&memory, socketOf, fail](Call& call)
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            const auto error = call.arg(1) == kSolSocket && call.arg(2) == kSoError && socket->failed() ? kWsaEConnRefused : 0;
            memory.write<std::uint32_t>(call.arg(3), error);
            if (call.arg(4))
            {
                memory.write<std::uint32_t>(call.arg(4), 4);
            }
            return ret(0);
        });
    add("ioctlsocket", 3, [&memory, socketOf, fail](Call& call)
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            if (call.arg(1) == kFionbio)
            {
                socket->nonBlocking = memory.read<std::uint32_t>(call.arg(2)) != 0;
            }
            else if (call.arg(1) == kFionread)
            {
                memory.write<std::uint32_t>(call.arg(2), socket->stream ? static_cast<std::uint32_t>(socket->stream->available()) : 0);
            }
            return ret(0);
        });

    // connect(s, sockaddr_in, length): { family, port (network order), IPv4 }
    add("connect", 3, [&p, &memory, socketOf, fail](Call& call) -> Outcome
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            if (socket->stream)
            {
                return fail(socket->settled() ? kWsaEIsConn : kWsaEAlready);
            }
            const auto address = call.arg(1);
            if (memory.read<std::uint16_t>(address) != kInet)
            {
                return fail(kWsaEAfNoSupport);
            }
            const auto port = static_cast<std::uint16_t>(swap16(memory.read<std::uint16_t>(address + 2)));
            auto       ip   = std::array<std::uint8_t, 4>{};
            memory.read(address + 4, ip);
            const auto dotted = std::format("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3]);
            p.trace.push_back(std::format("connect {}:{}", dotted, port));
            socket->stream = host::connect(dotted, port);
            if (socket->nonBlocking)
            {
                return fail(kWsaEWouldBlock); // completion shows in select and SO_ERROR
            }
            auto ready  = std::make_shared<Ready>();
            ready->test = [socket]()
            {
                return socket->settled();
            };
            return Block{ Wait{ { ready }, false, kInfinite }, [&p, socket](const WaitResult&)
                          {
                              if (socket->state() == host::SocketState::Open)
                              {
                                  return Return{ 0 };
                              }
                              p.setLastError(kWsaEConnRefused);
                              return Return{ kSocketError };
                          } };
        });

    add("send", 4, [&memory, socketOf, fail](Call& call) -> Outcome
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            if (!socket->stream)
            {
                return fail(kWsaENotConn);
            }
            switch (socket->state())
            {
                case host::SocketState::Connecting:
                    return fail(socket->nonBlocking ? kWsaEWouldBlock : kWsaENotConn);
                case host::SocketState::Open:
                    break;
                default:
                    return fail(kWsaEConnReset);
            }
            auto bytes = std::vector<std::uint8_t>(call.arg(2));
            memory.read(call.arg(1), bytes);
            socket->stream->send(bytes);
            return ret(call.arg(2));
        });

    add("recv", 4, [&p, &memory, socketOf, fail](Call& call) -> Outcome
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            if (!socket->stream)
            {
                return fail(kWsaENotConn);
            }
            const auto buffer = call.arg(1), size = call.arg(2);
            const auto take = [&p, &memory, socket, buffer, size]() -> Return
            {
                if (socket->stream->available() > 0)
                {
                    auto bytes = std::vector<std::uint8_t>(std::min<std::size_t>(size, socket->stream->available()));
                    socket->stream->receive(bytes);
                    memory.write(buffer, std::span<const std::uint8_t>{ bytes });
                    return Return{ static_cast<std::uint32_t>(bytes.size()) };
                }
                if (socket->state() == host::SocketState::Closed)
                {
                    return Return{ 0 };
                }
                p.setLastError(socket->state() == host::SocketState::Failed ? kWsaEConnReset : kWsaEWouldBlock);
                return Return{ kSocketError };
            };
            if (socket->nonBlocking || socket->readable())
            {
                return take();
            }
            auto ready  = std::make_shared<Ready>();
            ready->test = [socket]()
            {
                return socket->readable() || socket->state() == host::SocketState::Failed;
            };
            return Block{ Wait{ { ready }, false, kInfinite }, [take](const WaitResult&)
                          {
                              return take();
                          } };
        });

    add("closesocket", 1, [&p, socketOf, fail](Call& call)
        {
            const auto socket = socketOf(call.arg(0));
            if (!socket)
            {
                return fail(kWsaENotSock);
            }
            if (socket->stream)
            {
                socket->stream->close();
            }
            p.closeHandle(call.arg(0));
            return ret(0);
        });
    add("shutdown", 2, [](Call&)
        {
            return ret(0);
        });
    // sockaddr_in of the unspecified address
    for (const auto* name : { "getsockname", "getpeername" })
    {
        add(name, 3, [&memory](Call& call)
            {
                memory.fill(call.arg(1), 0, 16);
                memory.write<std::uint16_t>(call.arg(1), kInet);
                if (call.arg(2))
                {
                    memory.write<std::uint32_t>(call.arg(2), 16);
                }
                return ret(0);
            });
    }

    // __WSAFDIsSet(s, set)
    add("__WSAFDIsSet", 2, [readSet](Call& call)
        {
            const auto sockets = readSet(call.arg(1));
            return ret(std::ranges::find(sockets, call.arg(0)) != sockets.end() ? 1 : 0);
        });

    // select(n, readfds, writefds, exceptfds, timeout): rewrites each set to its ready sockets. polcore always
    // passes a zero timeout; a longer one parks the thread until a socket is ready or it runs out.
    add("select", 5, [&memory, socketOf, readSet](Call& call) -> Outcome
        {
            const auto sets = std::array{ call.arg(1), call.arg(2), call.arg(3) };
            const auto scan = [&memory, socketOf, readSet, sets](const bool write)
            {
                auto total = 0u;
                for (auto kind = 0u; kind < sets.size(); ++kind)
                {
                    auto ready = std::vector<std::uint32_t>{};
                    for (const auto handle : readSet(sets[kind]))
                    {
                        const auto socket = socketOf(handle);
                        if (socket && (kind == 0 ? socket->readable() : kind == 1 ? socket->writable()
                                                                                  : socket->failed()))
                        {
                            ready.push_back(handle);
                        }
                    }
                    total += static_cast<std::uint32_t>(ready.size());
                    if (write && sets[kind])
                    {
                        memory.write<std::uint32_t>(sets[kind], static_cast<std::uint32_t>(ready.size()));
                        for (auto i = 0u; i < ready.size(); ++i)
                        {
                            memory.write<std::uint32_t>(sets[kind] + 4 + 4 * i, ready[i]);
                        }
                    }
                }
                return total;
            };
            const auto timeout = call.arg(4);
            const auto ms      = timeout ? memory.read<std::uint32_t>(timeout) * 1000 + memory.read<std::uint32_t>(timeout + 4) / 1000 : kInfinite;
            if (scan(false) > 0 || ms == 0)
            {
                return ret(scan(true));
            }
            auto ready  = std::make_shared<Ready>();
            ready->test = [scan]()
            {
                return scan(false) > 0;
            };
            return Block{ Wait{ { ready }, false, ms }, [scan](const WaitResult&)
                          {
                              return Return{ scan(true) };
                          } };
        });
}

} // namespace tmweb::hle
