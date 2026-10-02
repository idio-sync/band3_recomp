#include "gocentral.h"

namespace band3::gocentral {

bool IsRockCentralHost(std::string_view host) {
    constexpr std::string_view kHmxServices = ".hmxservices.com";
    constexpr std::string_view kSandbox = "DummySandboxAddress.quazal.com";
    return (host.size() > kHmxServices.size() && host.ends_with(kHmxServices)) || host == kSandbox;
}

bool IsOwnAccountName(std::string_view username) {
    while (!username.empty() && username.front() == ' ') username.remove_prefix(1);
    while (!username.empty() && username.back() == ' ') username.remove_suffix(1);
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

}
