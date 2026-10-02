#include "online.h"

namespace band3::online {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

}  // namespace

bool IsRockCentralHost(std::string_view host) {
    constexpr std::string_view kHmxServices = ".hmxservices.com";
    constexpr std::string_view kSandbox = "DummySandboxAddress.quazal.com";
    return (host.size() > kHmxServices.size() && host.ends_with(kHmxServices)) || host == kSandbox;
}

bool IsOwnAccountName(std::string_view username) {
    username = Trim(username);
    if (username.empty()) return false;
    // GoCentral matches names ignoring case
    constexpr std::string_view kDefault = "user";
    if (username.size() != kDefault.size()) return true;
    for (size_t i = 0; i < kDefault.size(); i++) {
        const char c = username[i];
        if ((c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) != kDefault[i]) return true;
    }
    return false;
}

std::optional<Endpoint> ParseEndpoint(std::string_view text, uint16_t default_port) {
    text = Trim(text);
    const size_t colon = text.rfind(':');
    std::string_view host = text;
    uint32_t port = default_port;
    if (colon != std::string_view::npos) {
        host = text.substr(0, colon);
        const std::string_view digits = text.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return std::nullopt;
        port = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            port = port * 10 + static_cast<uint32_t>(c - '0');
        }
    }
    host = Trim(host);
    if (host.empty() || port == 0 || port > 65535) return std::nullopt;
    return Endpoint{std::string(host), static_cast<uint16_t>(port)};
}

std::optional<Endpoint> ParseJoinAddress(std::string_view text, uint16_t own_port) {
    auto join = ParseEndpoint(text, kGamePort);
    if (join && Trim(text).find(':') == std::string_view::npos &&
        (join->host == "127.0.0.1" || join->host == "localhost")) {
        join->port = own_port;
    }
    return join;
}

}
