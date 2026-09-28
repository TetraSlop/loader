// Non-game Win32 guest probe for Wine -> BoxedWine -> WebSocket -> Python TCP.
#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <winsock2.h>

namespace
{

auto connectTo(unsigned short port, bool expectedRejection = false, bool nonblocking = false) -> SOCKET
{
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (nonblocking)
    {
        int       size = 0, length = sizeof(size);
        const int result = getsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&size), &length);
        printf("PROBE OPTION receive buffer result=%d size=%d error=%d\n", result, size, result ? WSAGetLastError() : 0);
        u_long enabled = 1;
        ioctlsocket(socket, FIONBIO, &enabled);
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port        = htons(port);
    int connected           = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected && nonblocking && WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(socket, &writable);
        FD_SET(socket, &errors);
        timeval   timeout{ 10, 0 };
        const int ready = select(0, nullptr, &writable, &errors, &timeout);
        int       error = 0, length = sizeof(error);
        const int checked = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
        if (ready > 0 && !checked && !error && FD_ISSET(socket, &writable))
            connected = 0;
        else
            printf("PROBE FAIL asynchronous connect ready=%d checked=%d error=%d\n", ready, checked, error);
    }
    if (connected)
    {
        printf("PROBE %s connect %u error=%d\n", expectedRejection ? "EXPECTED" : "FAIL", port, WSAGetLastError());
        closesocket(socket);
        return INVALID_SOCKET;
    }
    if (nonblocking)
    {
        u_long enabled = 0;
        ioctlsocket(socket, FIONBIO, &enabled);
    }
    return socket;
}

} // namespace

auto main() -> int
{
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const SOCKET game = connectTo(51241, false, attempt == 1);
        if (game == INVALID_SOCKET)
            return 1;
        char line[1024]{};
        int  count = 0;
        while (count < 1023)
        {
            const int read = recv(game, line + count, 1, 0);
            if (read <= 0)
            {
                printf("PROBE FAIL game recv error=%d\n", WSAGetLastError());
                return 2;
            }
            if (line[count++] == '\n')
                break;
        }
        if (!strstr(line, "NOTICE AUTH"))
        {
            printf("PROBE FAIL game challenge %s\n", line);
            return 3;
        }
        printf("PROBE GAME CHALLENGE %d %s", attempt + 1, line);
        closesocket(game);
    }
    const SOCKET profile = connectTo(51220);
    if (profile == INVALID_SOCKET)
        return 4;
    char opening[40]{};
    opening[4] = 1;
    for (int i = 0; i < 40; ++i)
        if (send(profile, opening + i, 1, 0) != 1)
        {
            printf("PROBE FAIL profile send\n");
            return 5;
        }
    char reply[24]{};
    int  received = 0;
    while (received != 24)
    {
        const int count = recv(profile, reply + received, 24 - received, 0);
        if (count <= 0)
        {
            printf("PROBE FAIL profile recv error=%d\n", WSAGetLastError());
            return 6;
        }
        received += count;
    }
    printf("PROBE PROFILE OPEN %d bytes\n", received);
    closesocket(profile);
    const SOCKET rejected = connectTo(51242, true);
    if (rejected != INVALID_SOCKET)
    {
        printf("PROBE FAIL unconfigured route opened\n");
        return 7;
    }
    printf("PROBE PASS game reconnect, partial reads, profile partial writes, fixed-route rejection\n");
    WSACleanup();
    return 0;
}
