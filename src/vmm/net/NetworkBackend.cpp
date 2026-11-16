#include "NetworkBackend.h"

#include <cstdio>

namespace vmm {

std::string FormatMac(const MacAddress& mac)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

bool ParseMac(const std::string& text, MacAddress& mac)
{
    // "52:54:00:12:34:56" or "52-54-00-12-34-56".
    if (text.size() != 17) return false;
    const auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    MacAddress parsed{};
    for (std::size_t index = 0; index < 6; ++index) {
        const auto at = index * 3;
        const int high = hex(text[at]);
        const int low = hex(text[at + 1]);
        if (high < 0 || low < 0) return false;
        if (index < 5 && text[at + 2] != ':' && text[at + 2] != '-') return false;
        parsed[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    if ((parsed[0] & 1) != 0) return false; // A card address cannot be multicast.
    mac = parsed;
    return true;
}

} // namespace vmm
