#include "events.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>
#include <mutex>
#include <rex/logging.h>
#include "src/config.h"

namespace band3::events {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
#endif

// RB3E_EventHeader: 'RB3E' magic, then one byte each of version/type/size/platform
constexpr uint8_t kMagic[4] = {'R', 'B', '3', 'E'};
constexpr uint8_t kProtocolVersion = 0;
constexpr uint8_t kPlatformUnknown = 0xFF;  // RB3E_PLATFORM_UNKNOWN
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxPayload = 0xFF;        // RB3E_EVENTS_MAXPACKET

constexpr char kBuildTag[] = "band3_recomp";

std::mutex g_mutex;
socket_t g_socket = kNoSocket;
sockaddr_in g_dest{};
bool g_init_failed = false;
bool g_send_warned = false;

// opens the socket and resolves the target; called with g_mutex held
bool OpenSocket() {
    const auto& cfg = band3::GetConfig();

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        REXLOG_WARN("Events: WSAStartup failed, events disabled");
        return false;
    }
#endif

    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
        REXLOG_WARN("Events: could not create UDP socket, events disabled");
        return false;
    }

    // broadcast targets need SO_BROADCAST; sends must never stall the game
    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
#ifdef _WIN32
    u_long non_blocking = 1;
    ioctlsocket(s, FIONBIO, &non_blocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

    g_dest = {};
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons(static_cast<uint16_t>(cfg.events_port));
    if (inet_pton(AF_INET, cfg.events_target.c_str(), &g_dest.sin_addr) != 1) {
        REXLOG_WARN("Events: invalid target '{}', broadcasting instead", cfg.events_target);
        g_dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }

    char addr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &g_dest.sin_addr, addr, sizeof(addr));
    REXLOG_INFO("Events: sending RB3E events to {}:{}", addr, ntohs(g_dest.sin_port));

    g_socket = s;
    return true;
}

// called with g_mutex held
void SendLocked(EventType type, const void* data, size_t size) {
    if (size > kMaxPayload) size = kMaxPayload;

    uint8_t packet[kHeaderSize + kMaxPayload];
    std::memcpy(packet, kMagic, sizeof(kMagic));
    packet[4] = kProtocolVersion;
    packet[5] = type;
    packet[6] = static_cast<uint8_t>(size);
    packet[7] = kPlatformUnknown;
    if (size) std::memcpy(packet + kHeaderSize, data, size);

    int sent = sendto(g_socket, reinterpret_cast<const char*>(packet),
                      static_cast<int>(kHeaderSize + size), 0,
                      reinterpret_cast<const sockaddr*>(&g_dest), sizeof(g_dest));
    if (sent < 0 && !g_send_warned) {
        g_send_warned = true;
        REXLOG_WARN("Events: send failed; further send errors are not logged");
    }
}

}

bool Enabled() {
    return band3::GetConfig().events_enabled;
}

void SendString(EventType type, const char* str) {
    if (!str) return;
    size_t len = 0;
    while (len < kMaxPayload && str[len] != '\0') len++;
    Send(type, str, len);
}

void Send(EventType type, const void* data, size_t size) {
    if (!Enabled()) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_failed) return;
    if (g_socket == kNoSocket) {
        if (!OpenSocket()) {
            g_init_failed = true;
            return;
        }
        // like RB3E, announce ourselves once when the socket opens
        SendLocked(kAlive, kBuildTag, sizeof(kBuildTag));
    }
    SendLocked(type, data, size);
}

}
