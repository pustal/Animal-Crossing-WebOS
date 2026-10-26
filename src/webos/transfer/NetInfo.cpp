/**
 * @file NetInfo.cpp
 * @brief Implementation of the network helpers (see NetInfo.h).
 */
#include "NetInfo.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace acwebos
{
namespace
{

/** Lower rank wins: wired first, then Wi-Fi, then anything else. */
int InterfaceRank(const char *name)
{
    if (std::strncmp(name, "eth", 3) == 0)
        return 0;
    if (std::strncmp(name, "wlan", 4) == 0)
        return 1;
    return 2;
}

/** Converts a dotted-quad string to a host-order integer. @return False on failure. */
bool ParseIPv4(const std::string &text, std::uint32_t *out)
{
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1)
        return false;
    *out = ntohl(addr.s_addr);
    return true;
}

} // namespace

std::string FindLanAddress()
{
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
        return {};

    std::string best;
    int bestRank = 3;

    for (const ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
            continue;

        const int rank = InterfaceRank(entry->ifa_name);
        if (rank >= bestRank)
            continue;

        std::array<char, INET_ADDRSTRLEN> buffer{};
        const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer.data(), static_cast<socklen_t>(buffer.size()))) {
            best     = buffer.data();
            bestRank = rank;
        }
    }

    freeifaddrs(interfaces);
    return best;
}

bool IsPrivateNetworkAddress(const std::string &address)
{
    std::uint32_t ip = 0;
    if (ParseIPv4(address, &ip)) {
        const auto inRange = [ip](std::uint32_t network, int prefixBits) {
            const std::uint32_t mask = prefixBits == 0 ? 0u : ~0u << (32 - prefixBits);
            return (ip & mask) == (network & mask);
        };
        return inRange(0x7F000000u, 8)       // 127.0.0.0/8 loopback
               || inRange(0x0A000000u, 8)    // 10.0.0.0/8
               || inRange(0xAC100000u, 12)   // 172.16.0.0/12
               || inRange(0xC0A80000u, 16)   // 192.168.0.0/16
               || inRange(0xA9FE0000u, 16);  // 169.254.0.0/16 link-local
    }

    // IPv6 (the server listens on IPv4, but stay correct if that changes).
    in6_addr addr6{};
    if (inet_pton(AF_INET6, address.c_str(), &addr6) == 1) {
        const std::uint8_t first = addr6.s6_addr[0];
        const std::uint8_t second = addr6.s6_addr[1];
        return IN6_IS_ADDR_LOOPBACK(&addr6)                  // ::1
               || (first == 0xFE && (second & 0xC0) == 0x80) // fe80::/10 link-local
               || (first & 0xFE) == 0xFC;                    // fc00::/7 unique-local
    }

    return false;
}

} // namespace acwebos
