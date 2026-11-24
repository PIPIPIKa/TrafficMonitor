#include "../TrafficMonitor/TrafficScope.h"
#include <cassert>
#include <vector>

using namespace traffic_scope;

int main()
{
    Interface adapter;
    adapter.mac = {2, 0, 0, 0, 0, 1};
    Prefix prefix;
    prefix.address[0] = 0x20;
    prefix.address[1] = 0x01;
    prefix.address[2] = 0x0d;
    prefix.address[3] = 0xb8;
    prefix.size = 16;
    prefix.bits = 65;
    adapter.prefixes.push_back(prefix);

    Counts result;
    std::vector<uint8_t> ethernet(64);
    auto* frame = ethernet.data();
    std::copy(adapter.mac.begin(), adapter.mac.end(), frame);
    frame[12] = 0x86; frame[13] = 0xdd; frame[14] = 0x60;
    frame[22] = 0xfe; frame[23] = 0x80;
    count(adapter, frame, 54, 1514, result); // IPv6 printer download
    assert(result.lanDown == 1514 && result.wanDown == 0);
    frame[22] = 0x26; frame[23] = 0x06;
    count(adapter, frame, 54, 1200, result);
    assert(result.wanDown == 1200);

    ethernet.assign(64, 0);
    frame = ethernet.data();
    std::copy(adapter.mac.begin(), adapter.mac.end(), frame + 6);
    frame[12] = 0x81; frame[13] = 0; frame[16] = 8; frame[18] = 0x45;
    frame[34] = 192; frame[35] = 168;
    count(adapter, frame, 38, 1024, result);
    assert(result.lanUp == 1024);
    frame[34] = 8; frame[35] = 8;
    count(adapter, frame, 38, 128, result);
    assert(result.wanUp == 128);
    count(adapter, frame, 37, 128, result);
    assert(result.wanUp == 128); // truncated IP header
    frame[0] = 0xff;
    count(adapter, frame, 38, 96, result);
    assert(result.lanUp == 1120); // Ethernet broadcast
    auto peer = prefix.address;
    assert(localPeer(adapter, peer.data(), 16));
    peer[8] = 0x80;
    assert(!localPeer(adapter, peer.data(), 16));
}

