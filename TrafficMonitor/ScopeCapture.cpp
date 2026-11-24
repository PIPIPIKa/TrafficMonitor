// This translation unit uses the Windows networking headers independently of MFC.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "ScopeCapture.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")

namespace
{
// Minimal C ABI of the optional system-installed Npcap library.
struct Pcap;
struct Header { long seconds, microseconds; unsigned captured, original; };
using Callback = void (__cdecl *)(unsigned char*, const Header*, const unsigned char*);
struct Api
{
    HMODULE module{};
    Pcap* (__cdecl *open)(const char*, int, int, int, char*){};
    int (__cdecl *link)(Pcap*){};
    int (__cdecl *nonblock)(Pcap*, int, char*){};
    int (__cdecl *dispatch)(Pcap*, int, Callback, unsigned char*){};
    HANDLE (__cdecl *event)(Pcap*){};
    void (__cdecl *close)(Pcap*){};

    Api()
    {
        wchar_t system[MAX_PATH]{};
        auto length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;
        auto path = std::wstring(system) + L"\\Npcap\\wpcap.dll";
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return;
        open = reinterpret_cast<decltype(open)>(GetProcAddress(module, "pcap_open_live"));
        link = reinterpret_cast<decltype(link)>(GetProcAddress(module, "pcap_datalink"));
        nonblock = reinterpret_cast<decltype(nonblock)>(GetProcAddress(module, "pcap_setnonblock"));
        dispatch = reinterpret_cast<decltype(dispatch)>(GetProcAddress(module, "pcap_dispatch"));
        event = reinterpret_cast<decltype(event)>(GetProcAddress(module, "pcap_getevent"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(module, "pcap_close"));
    }
    bool ready() const { return open && link && nonblock && dispatch && event && close; }
    ~Api() { if (module) FreeLibrary(module); }
};

struct Reader
{
    Api& api;
    traffic_scope::Interface adapter;
    std::string name;
    Pcap* handle{};
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex guard;
    traffic_scope::Counts pending;
    traffic_scope::Counts batch; // Owned by the capture thread.

    Reader(Api& library, const IP_ADAPTER_ADDRESSES& nic) : api(library), name(nic.AdapterName)
    {
        std::copy_n(nic.PhysicalAddress, 6, adapter.mac.begin());
        for (auto* entry = nic.FirstUnicastAddress; entry; entry = entry->Next)
        {
            auto* addr = entry->Address.lpSockaddr;
            if (!addr || (addr->sa_family != AF_INET && addr->sa_family != AF_INET6))
                continue;
            traffic_scope::Prefix prefix;
            prefix.bits = entry->OnLinkPrefixLength;
            if (addr->sa_family == AF_INET)
            {
                prefix.size = 4;
                memcpy(prefix.address.data(), &reinterpret_cast<sockaddr_in*>(addr)->sin_addr, 4);
            }
            else
            {
                prefix.size = 16;
                memcpy(prefix.address.data(), &reinterpret_cast<sockaddr_in6*>(addr)->sin6_addr, 16);
            }
            adapter.prefixes.push_back(prefix);
        }
    }

    bool start()
    {
        char error[256]{};
        std::string device = "\\Device\\NPF_" + name;
        handle = api.open(device.c_str(), 96, 0, 100, error);
        if (!handle || api.link(handle) != 1 || api.nonblock(handle, 1, error) < 0)
            return false;
        HANDLE wake = api.event(handle);
        if (!wake || wake == INVALID_HANDLE_VALUE)
            return false;
        running = true;
        worker = std::thread([this, wake] {
            while (running)
            {
                int n = api.dispatch(handle, 1024, onFrame, reinterpret_cast<unsigned char*>(this));
                if (n < 0)
                    break;
                if (n == 0)
                {
                    if (WaitForSingleObject(wake, 100) == WAIT_FAILED)
                        break;
                    continue;
                }
                std::lock_guard<std::mutex> lock(guard);
                pending.add(batch);
                batch = {};
            }
            running = false;
        });
        return true;
    }
    static void __cdecl onFrame(unsigned char* arg, const Header* h, const unsigned char* frame)
    {
        auto& reader = *reinterpret_cast<Reader*>(arg);
        traffic_scope::count(reader.adapter, frame, h->captured, h->original, reader.batch);
    }
    traffic_scope::Counts drain()
    {
        std::lock_guard<std::mutex> lock(guard);
        auto result = pending;
        pending = {};
        return result;
    }
    ~Reader()
    {
        running = false;
        if (worker.joinable())
            worker.join();
        if (handle)
            api.close(handle);
    }
};
bool sameInterface(const Reader& a, const Reader& b)
{
    if (a.name != b.name || a.adapter.mac != b.adapter.mac ||
        a.adapter.prefixes.size() != b.adapter.prefixes.size())
        return false;
    for (size_t i = 0; i < a.adapter.prefixes.size(); ++i)
    {
        const auto& x = a.adapter.prefixes[i];
        const auto& y = b.adapter.prefixes[i];
        if (x.address != y.address || x.bits != y.bits || x.size != y.size)
            return false;
    }
    return true;
}
}

struct ScopeCapture::Impl
{
    Api api;
    std::map<ULONG, std::unique_ptr<Reader>> readers;
    std::set<ULONG> selected;
    bool all{};
    ULONGLONG nextRefresh{};
    std::atomic<bool> active{false};

    void refresh()
    {
        ULONG len = 16384;
        std::vector<unsigned char> buf(len);
        ULONG status;
        do
        {
            buf.resize(len);
            status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
        } while (status == ERROR_BUFFER_OVERFLOW);
        if (status != NO_ERROR)
        {
            readers.clear();
            return;
        }
        std::set<ULONG> found;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next)
        {
            ULONG index = a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
            if (!selected.count(index) || a->OperStatus != IfOperStatusUp || a->PhysicalAddressLength != 6)
                continue;
            if (all)
            {
                MIB_IF_ROW2 row{};
                row.InterfaceLuid = a->Luid;
                if (GetIfEntry2(&row) != NO_ERROR || !row.InterfaceAndOperStatusFlags.HardwareInterface)
                    continue; // Do not count both a VPN/virtual switch and its physical transport.
            }
            found.insert(index);
            auto fresh = std::make_unique<Reader>(api, *a);
            auto it = readers.find(index);
            if (it != readers.end() && it->second->running && sameInterface(*it->second, *fresh))
                continue;
            readers.erase(index);
            if (fresh->start())
                readers.emplace(index, std::move(fresh));
        }
        for (auto it = readers.begin(); it != readers.end();)
            if (!found.count(it->first)) it = readers.erase(it); else ++it;
    }
};

ScopeCapture::ScopeCapture() : state(std::make_unique<Impl>()) {}
ScopeCapture::~ScopeCapture() = default;
bool ScopeCapture::active() const { return state->active.load(); }

traffic_scope::Counts ScopeCapture::sample(const std::set<ULONG>& indexes, bool allInterfaces)
{
    traffic_scope::Counts bytes;
    if (!state->api.ready())
        return bytes;
    bool active = false;
    for (const auto& entry : state->readers)
    {
        bytes.add(entry.second->drain());
        active |= entry.second->running.load();
    }
    auto now = GetTickCount64();
    if (indexes != state->selected || allInterfaces != state->all || now >= state->nextRefresh)
    {
        state->selected = indexes;
        state->all = allInterfaces;
        state->refresh();
        state->nextRefresh = now + 15000;
        active = !state->readers.empty();
    }
    state->active = active;
    return bytes;
}

