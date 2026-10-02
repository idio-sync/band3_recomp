#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xsocket.h>
#include <rex/types.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "generated/band3_init.h"
#include "src/Net/native_socket.h"
#include "src/settings.h"
#ifdef _WIN32
#include <windows.h>
#endif

// RB3's networking (Quazal, for Rock Central and online play) through the
// SDK's NetDll exports. Quazal needs some the SDK doesn't do as a console
// does, which band3 does on the host instead:
//   - XNetDnsLookup, which finds nothing in the SDK;
//   - getsockname, a stub there, which Quazal asks for its socket's port;
//   - its overlapped UDP: it posts a WSARecvFrom and polls
//     WSAGetOverlappedResult until a datagram lands, sending with WSASendTo.
//     The SDK reports each receive done at once with nothing in it, so
//     Quazal never hears back.
// The rest go on to the SDK's. With log_net_calls on, each is logged.

namespace {

using Export = void(PPCContext&, uint8_t*);

Export* SdkExport(const char* name) {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto* fn = runtime ? reinterpret_cast<Export*>(GetProcAddress(runtime, name)) : nullptr;
    if (!fn) REXLOG_ERROR("net: {} has no {}", BAND3_REXRUNTIME_DLL, name);
    return fn;
#else
    return nullptr;
#endif
}

// Quazal polls some of these in a loop: each one's first 200 calls are
// logged, then every 10000th
bool ShouldLog(std::atomic<uint64_t>& calls) {
    if (!REXCVAR_GET(log_net_calls)) return false;
    const uint64_t n = calls.fetch_add(1);
    return n < 200 || n % 10000 == 0;
}

void Forward(Export* sdk, const char* name, std::atomic<uint64_t>& calls, PPCContext& ctx,
             uint8_t* base) {
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32,
                   r7 = ctx.r7.u32;
    if (sdk) {
        sdk(ctx, base);
    } else {
        ctx.r3.u64 = static_cast<uint64_t>(-1);
    }
    if (ShouldLog(calls)) {
        REXLOG_INFO("net: {}({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) -> {:#x}", name, r3, r4, r5, r6, r7,
                    ctx.r3.u32);
    }
}

std::string Endpoint(uint32_t address, uint16_t port) {
    // both in network order
    return fmt::format("{}.{}.{}.{}:{}", address & 0xFF, (address >> 8) & 0xFF,
                       (address >> 16) & 0xFF, address >> 24,
                       static_cast<uint16_t>((port >> 8) | (port << 8)));
}

constexpr uint32_t kSocketErrorResult = 0xFFFFFFFF;
// Winsock errors
constexpr uint32_t kIoIncomplete = 996;   // WSA_IO_INCOMPLETE
constexpr uint32_t kIoPending = 997;      // WSA_IO_PENDING
constexpr uint32_t kInvalid = 10022;      // WSAEINVAL
constexpr uint32_t kWouldBlock = 10035;   // WSAEWOULDBLOCK
constexpr uint32_t kNotSocket = 10038;    // WSAENOTSOCK
constexpr uint32_t kNetworkDown = 10050;  // WSAENETDOWN
constexpr uint32_t kNoBuffers = 10055;    // WSAENOBUFS
constexpr uint32_t kHostNotFound = 11001; // WSAHOST_NOT_FOUND
constexpr uint32_t kStatusPending = 0x103;
// WSAOVERLAPPED: Internal (status), InternalHigh (bytes), Offset, OffsetHigh, hEvent
constexpr uint32_t kOverlappedStatus = 0x0;
constexpr uint32_t kOverlappedBytes = 0x4;
constexpr uint32_t kOverlappedEvent = 0x10;
// a NetDll call's 9th argument (lpOverlapped for WSASendTo and WSARecvFrom),
// on the caller's stack
constexpr uint32_t kNinthArgument = 0x54;
// the guest's sockaddr_in: family (big-endian), port and address (network
// order), padding
constexpr uint32_t kSockaddrSize = 16;

void SetLastError(PPCContext& ctx, uint8_t* base, uint32_t error) {
    static Export* const sdk = SdkExport("__imp__NetDll_WSASetLastError");
    if (!sdk) return;
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = error;
    sdk(frame, base);
}

uint64_t NativeSocket(uint32_t handle) {
    auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(handle);
    return socket ? socket->native_handle() : ~uint64_t{0};
}

void WriteSockaddr(uint8_t* base, uint32_t sockaddr, uint32_t address, uint16_t port) {
    uint8_t* out = base + sockaddr;
    out[0] = 0;
    out[1] = 2;  // AF_INET
    std::memcpy(out + 2, &port, 2);
    std::memcpy(out + 4, &address, 4);
    std::memset(out + 8, 0, 8);
}

void SetEvent(uint32_t event) {
    if (!event) return;
    if (auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) e->Set(0, false);
}

void Complete(uint8_t* base, uint32_t overlapped, uint32_t bytes) {
    REX_STORE_U32(overlapped + kOverlappedStatus, 0);
    REX_STORE_U32(overlapped + kOverlappedBytes, bytes);
    SetEvent(REX_LOAD_U32(overlapped + kOverlappedEvent));
}

struct Receive {
    uint32_t socket, buffers, buffer_count, bytes, flags, from, from_length;
};

std::mutex g_pending_mutex;
// receives waiting for a datagram, by their WSAOVERLAPPED
std::unordered_map<uint32_t, Receive> g_pending;

// a datagram for `receive`, into its guest buffers: its length, or
// kNothingWaiting / kSocketError
int TakeDatagram(uint8_t* base, const Receive& receive, uint64_t native, int wait_ms) {
    static thread_local std::vector<uint8_t> datagram(65536);
    uint32_t address = 0;
    uint16_t port = 0;
    const int length =
        band3::net::ReceiveFrom(native, datagram.data(), datagram.size(), address, port, wait_ms);
    if (length < 0) return length;
    // WSABUF: length, then the buffer
    uint32_t copied = 0;
    for (uint32_t i = 0; i < receive.buffer_count && copied < static_cast<uint32_t>(length); i++) {
        const uint32_t size = REX_LOAD_U32(receive.buffers + 8 * i);
        const uint32_t buffer = REX_LOAD_U32(receive.buffers + 8 * i + 4);
        const uint32_t take = std::min(size, static_cast<uint32_t>(length) - copied);
        std::memcpy(base + buffer, datagram.data() + copied, take);
        copied += take;
    }
    if (receive.bytes) REX_STORE_U32(receive.bytes, copied);
    if (receive.flags) REX_STORE_U32(receive.flags, 0);
    if (receive.from && receive.from_length && REX_LOAD_U32(receive.from_length) >= kSockaddrSize) {
        WriteSockaddr(base, receive.from, address, port);
        REX_STORE_U32(receive.from_length, kSockaddrSize);
    }
    static std::atomic<uint64_t> calls{0};
    if (ShouldLog(calls)) {
        REXLOG_INFO("net: received {} bytes from {}", copied, Endpoint(address, port));
    }
    return static_cast<int>(copied);
}

}  // namespace

#define BAND3_NET_CALL(name)                                   \
    extern "C" REX_FUNC(__imp__##name) {                       \
        static Export* const sdk = SdkExport("__imp__" #name); \
        static std::atomic<uint64_t> calls{0};                 \
        Forward(sdk, #name, calls, ctx, base);                 \
    }

BAND3_NET_CALL(NetDll_WSAStartup)
BAND3_NET_CALL(NetDll_WSAWaitForMultipleEvents)
BAND3_NET_CALL(NetDll_WSACreateEvent)
BAND3_NET_CALL(NetDll_XNetStartup)
BAND3_NET_CALL(NetDll_XNetGetTitleXnAddr)
BAND3_NET_CALL(NetDll_XNetGetConnectStatus)
BAND3_NET_CALL(NetDll_XNetConnect)
BAND3_NET_CALL(NetDll_XNetServerToInAddr)
BAND3_NET_CALL(NetDll_XNetXnAddrToInAddr)
BAND3_NET_CALL(NetDll_XNetRegisterKey)
BAND3_NET_CALL(NetDll_XNetQosLookup)
BAND3_NET_CALL(NetDll_socket)
BAND3_NET_CALL(NetDll_bind)
BAND3_NET_CALL(NetDll_connect)
BAND3_NET_CALL(NetDll_setsockopt)
BAND3_NET_CALL(NetDll_ioctlsocket)
BAND3_NET_CALL(NetDll_select)
BAND3_NET_CALL(NetDll_recvfrom)
BAND3_NET_CALL(NetDll_sendto)

// NetDll_XNetDnsLookup(caller, const char* host, event, XNDNS** result),
// looked up on the host before it returns, which Quazal, polling the result,
// takes as a lookup that finished at once
extern "C" REX_FUNC(__imp__NetDll_XNetDnsLookup) {
    // XNDNS: status, address count, then up to 8 IPv4 addresses
    constexpr uint32_t kMaxAddresses = 8;
    constexpr uint32_t kSize = 8 + 4 * kMaxAddresses;
    const uint32_t host_ptr = ctx.r4.u32, event = ctx.r5.u32, result = ctx.r6.u32;
    if (!host_ptr || !result) {
        ctx.r3.u64 = kInvalid;
        return;
    }
    const std::string host(reinterpret_cast<const char*>(base + host_ptr),
                           strnlen(reinterpret_cast<const char*>(base + host_ptr), 256));
    const auto addresses = band3::net::ResolveIPv4(host);
    const uint32_t dns = rex::system::kernel_memory()->SystemHeapAlloc(kSize, 4);
    if (!dns) {
        ctx.r3.u64 = kNoBuffers;
        return;
    }
    std::memset(base + dns, 0, kSize);
    const auto count = static_cast<uint32_t>(std::min<size_t>(addresses.size(), kMaxAddresses));
    REX_STORE_U32(dns, count ? 0 : kHostNotFound);
    REX_STORE_U32(dns + 4, count);
    // network order, as the guest keeps an IN_ADDR
    for (uint32_t i = 0; i < count; i++) std::memcpy(base + dns + 8 + 4 * i, &addresses[i], 4);
    REX_STORE_U32(result, dns);
    SetEvent(event);
    ctx.r3.u64 = 0;
    static std::atomic<uint64_t> calls{0};
    if (ShouldLog(calls)) {
        REXLOG_INFO("net: DNS {} -> {}", host,
                    count ? Endpoint(addresses[0], 0) + fmt::format(" ({} found)", count)
                          : std::string("not found"));
    }
}

// NetDll_XNetDnsRelease(caller, XNDNS*): frees what band3's lookup gave
extern "C" REX_FUNC(__imp__NetDll_XNetDnsRelease) {
    if (ctx.r4.u32) rex::system::kernel_memory()->SystemHeapFree(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// NetDll_getsockname(caller, socket, sockaddr* name, int* name_length)
extern "C" REX_FUNC(__imp__NetDll_getsockname) {
    const uint32_t socket = ctx.r4.u32, name = ctx.r5.u32, name_length = ctx.r6.u32;
    const uint64_t native = NativeSocket(socket);
    uint16_t port = 0;
    uint32_t address = 0;
    if (native != ~uint64_t{0} && name && name_length &&
        REX_LOAD_U32(name_length) >= kSockaddrSize &&
        band3::net::BoundAddress(native, port, address)) {
        WriteSockaddr(base, name, address, port);
        REX_STORE_U32(name_length, kSockaddrSize);
        ctx.r3.u64 = 0;
    } else {
        SetLastError(ctx, base, kInvalid);
        ctx.r3.u64 = kSocketErrorResult;
    }
    static std::atomic<uint64_t> calls{0};
    if (ShouldLog(calls)) {
        REXLOG_INFO("net: getsockname({:#x}) -> {}", socket,
                    ctx.r3.u32 ? std::string("failed") : Endpoint(address, port));
    }
}

// NetDll_WSARecvFrom(caller, socket, WSABUF* buffers, count, DWORD* bytes,
// DWORD* flags, sockaddr* from, int* from_length, WSAOVERLAPPED*, routine)
extern "C" REX_FUNC(__imp__NetDll_WSARecvFrom) {
    const Receive receive{ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                          ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + kNinthArgument);
    const uint64_t native = NativeSocket(receive.socket);
    ctx.r3.u64 = kSocketErrorResult;
    if (native == ~uint64_t{0}) {
        SetLastError(ctx, base, kNotSocket);
        return;
    }
    const int length = TakeDatagram(base, receive, native, 0);
    if (length >= 0) {
        if (overlapped) Complete(base, overlapped, static_cast<uint32_t>(length));
        ctx.r3.u64 = 0;
        return;
    }
    if (length == band3::net::kSocketError) {
        SetLastError(ctx, base, kNetworkDown);
        return;
    }
    if (!overlapped) {
        SetLastError(ctx, base, kWouldBlock);
        return;
    }
    REX_STORE_U32(overlapped + kOverlappedStatus, kStatusPending);
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending[overlapped] = receive;
    }
    SetLastError(ctx, base, kIoPending);
}

// NetDll_WSAGetOverlappedResult(caller, socket, WSAOVERLAPPED*, DWORD* bytes,
// BOOL wait, DWORD* flags)
extern "C" REX_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
    const uint32_t overlapped = ctx.r5.u32, bytes = ctx.r6.u32, flags = ctx.r8.u32;
    const bool wait = ctx.r7.u32 != 0;
    Receive receive{};
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        if (auto it = g_pending.find(overlapped); it != g_pending.end()) {
            receive = it->second;
            pending = true;
        }
    }
    if (!pending) {
        // finished already: a send, or a receive that found its datagram
        if (bytes) REX_STORE_U32(bytes, REX_LOAD_U32(overlapped + kOverlappedBytes));
        if (flags) REX_STORE_U32(flags, 0);
        ctx.r3.u64 = 1;
        return;
    }
    const uint64_t native = NativeSocket(receive.socket);
    int length = band3::net::kSocketError;
    if (native != ~uint64_t{0}) {
        do {
            length = TakeDatagram(base, receive, native, wait ? 100 : 0);
        } while (wait && length == band3::net::kNothingWaiting);
    }
    if (length == band3::net::kNothingWaiting) {
        SetLastError(ctx, base, kIoIncomplete);
        ctx.r3.u64 = 0;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.erase(overlapped);
    }
    if (length == band3::net::kSocketError) {
        REX_STORE_U32(overlapped + kOverlappedStatus, kNetworkDown);
        SetLastError(ctx, base, kNetworkDown);
        ctx.r3.u64 = 0;
        return;
    }
    Complete(base, overlapped, static_cast<uint32_t>(length));
    if (bytes) REX_STORE_U32(bytes, static_cast<uint32_t>(length));
    if (flags) REX_STORE_U32(flags, 0);
    ctx.r3.u64 = 1;
}

// NetDll_WSASendTo(caller, socket, WSABUF* buffers, count, DWORD* bytes, flags,
// const sockaddr* to, to_length, WSAOVERLAPPED*, routine)
extern "C" REX_FUNC(__imp__NetDll_WSASendTo) {
    const uint32_t socket = ctx.r4.u32, buffers = ctx.r5.u32, count = ctx.r6.u32,
                   bytes = ctx.r7.u32, to = ctx.r9.u32, to_length = ctx.r10.u32;
    const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + kNinthArgument);
    const uint64_t native = NativeSocket(socket);
    ctx.r3.u64 = kSocketErrorResult;
    if (native == ~uint64_t{0} || !to || to_length < kSockaddrSize) {
        SetLastError(ctx, base, native == ~uint64_t{0} ? kNotSocket : kInvalid);
        return;
    }
    std::vector<uint8_t> datagram;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t size = REX_LOAD_U32(buffers + 8 * i);
        const uint8_t* buffer = base + REX_LOAD_U32(buffers + 8 * i + 4);
        datagram.insert(datagram.end(), buffer, buffer + size);
    }
    uint16_t port = 0;
    uint32_t address = 0;
    std::memcpy(&port, base + to + 2, 2);
    std::memcpy(&address, base + to + 4, 4);
    const int sent = band3::net::SendTo(native, datagram.data(), datagram.size(), address, port);
    static std::atomic<uint64_t> calls{0};
    if (ShouldLog(calls)) {
        REXLOG_INFO("net: sent {} of {} bytes to {}", sent, datagram.size(), Endpoint(address, port));
    }
    if (sent < 0) {
        SetLastError(ctx, base, kNetworkDown);
        return;
    }
    if (bytes) REX_STORE_U32(bytes, static_cast<uint32_t>(sent));
    if (overlapped) Complete(base, overlapped, static_cast<uint32_t>(sent));
    ctx.r3.u64 = 0;
}

// receives still waiting on a socket go with it
extern "C" REX_FUNC(__imp__NetDll_closesocket) {
    static Export* const sdk = SdkExport("__imp__NetDll_closesocket");
    static std::atomic<uint64_t> calls{0};
    const uint32_t socket = ctx.r4.u32;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        std::erase_if(g_pending, [socket](const auto& p) { return p.second.socket == socket; });
    }
    Forward(sdk, "NetDll_closesocket", calls, ctx, base);
}
