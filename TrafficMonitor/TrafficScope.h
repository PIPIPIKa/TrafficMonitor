#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace traffic_scope
{
struct Counts
{
    uint64_t lanUp{}, lanDown{}, wanUp{}, wanDown{};

    void add(const Counts& other)
    {
        lanUp += other.lanUp;
        lanDown += other.lanDown;
        wanUp += other.wanUp;
        wanDown += other.wanDown;
    }
};

struct Prefix
{
    std::array<uint8_t, 16> address{};
    unsigned size{}, bits{};

    bool matches(const uint8_t* peer, unsigned length) const
    {
        if (size != length || !bits || bits > size * 8)
            return false;
        unsigned bytes = bits / 8, rest = bits % 8;
        return std::memcmp(address.data(), peer, bytes) == 0 &&
            (!rest || ((address[bytes] ^ peer[bytes]) >> (8 - rest)) == 0);
    }
};

struct Interface
{
    std::array<uint8_t, 6> mac{};
    std::vector<Prefix> prefixes;
};

inline bool localPeer(const Interface& adapter, const uint8_t* p, unsigned size)
{
    if (size == 4 && (p[0] == 10 || p[0] == 127 ||
        (p[0] == 172 && (p[1] & 0xf0) == 16) || (p[0] == 192 && p[1] == 168) ||
        (p[0] == 169 && p[1] == 254) || (p[0] == 100 && (p[1] & 0xc0) == 64) ||
        (p[0] >= 224 && p[0] <= 239)))
        return true;
    if (size == 16 && (p[0] == 0xff || (p[0] & 0xfe) == 0xfc ||
        (p[0] == 0xfe && (p[1] & 0xc0) == 0x80)))
        return true;
    for (const auto& prefix : adapter.prefixes)
        if (prefix.matches(p, size))
            return true;
    return false;
}

// Count original frame length even when only the link and network headers were captured.
inline void count(const Interface& adapter, const uint8_t* frame, unsigned captured,
                  unsigned original, Counts& result)
{
    if (captured < 14 || captured > original)
        return;
    const bool up = std::memcmp(frame + 6, adapter.mac.data(), 6) == 0;
    const bool group = (frame[0] & 1) != 0;
    if (!up && !group && std::memcmp(frame, adapter.mac.data(), 6) != 0)
        return;
    unsigned offset = 14;
    unsigned type = frame[12] * 256u + frame[13];
    while (type == 0x8100 || type == 0x88a8)
    {
        if (captured < offset + 4)
            return;
        type = frame[offset + 2] * 256u + frame[offset + 3];
        offset += 4;
    }
    bool lan = true; // ARP and other non-IP Ethernet protocols stay on the link.
    if (!group && (type == 0x0800 || type == 0x86dd))
    {
        const bool ipv4 = type == 0x0800;
        if (captured < offset + (ipv4 ? 20u : 40u) || frame[offset] >> 4 != (ipv4 ? 4 : 6))
            return;
        const auto* peer = frame + offset + (ipv4 ? (up ? 16 : 12) : (up ? 24 : 8));
        lan = localPeer(adapter, peer, ipv4 ? 4 : 16);
    }
    auto& bytes = lan ? (up ? result.lanUp : result.lanDown) :
                        (up ? result.wanUp : result.wanDown);
    bytes += original;
}
}

