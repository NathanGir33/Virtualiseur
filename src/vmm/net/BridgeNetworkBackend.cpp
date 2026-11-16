#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "BridgeNetworkBackend.h"

#include "../core/Log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <type_traits>
#include <stdexcept>
#include <string>

#pragma comment(lib, "Iphlpapi.lib")

namespace vmm {
namespace {

constexpr std::uint16_t kTypeIpv4 = 0x0800;
constexpr std::uint16_t kTypeArp = 0x0806;
constexpr std::uint16_t kTypeIpv6 = 0x86DD;
constexpr std::size_t kMaxFrame = 65536;

// Mirrors of the pcap ABI (wpcap.dll, cdecl).
struct PcapPacketHeader {
    long seconds;
    long microseconds;
    std::uint32_t captured;
    std::uint32_t length;
};
struct BpfProgram {
    unsigned int length;
    void* instructions;
};

std::uint16_t Load16(const std::uint8_t* data) { return static_cast<std::uint16_t>((data[0] << 8) | data[1]); }
std::uint32_t LoadIp(const std::uint8_t* data)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data, 4);
    return value;
}

std::string FormatIp(std::uint32_t address)
{
    char text[INET_ADDRSTRLEN] = {};
    in_addr value{};
    value.s_addr = address;
    InetNtopA(AF_INET, &value, text, sizeof(text));
    return text;
}

std::string Narrow(const wchar_t* text)
{
    if (text == nullptr || *text == 0) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

bool SameText(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

struct HostAdapter {
    std::string guid;
    std::string friendlyName;
    std::string description;
    MacAddress mac{};
    bool wireless = false;
    bool up = false;
    bool gateway = false;
};

std::vector<HostAdapter> HostAdapters()
{
    std::vector<HostAdapter> result;
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                      GAA_FLAG_INCLUDE_GATEWAYS,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) return result;
    for (auto* item = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); item != nullptr; item = item->Next) {
        if ((item->IfType != IF_TYPE_ETHERNET_CSMACD && item->IfType != IF_TYPE_IEEE80211) ||
            item->PhysicalAddressLength != 6) {
            continue;
        }
        HostAdapter adapter;
        adapter.guid = item->AdapterName;
        adapter.friendlyName = Narrow(item->FriendlyName);
        adapter.description = Narrow(item->Description);
        std::memcpy(adapter.mac.data(), item->PhysicalAddress, 6);
        adapter.wireless = item->IfType == IF_TYPE_IEEE80211;
        adapter.up = item->OperStatus == IfOperStatusUp;
        adapter.gateway = item->FirstGatewayAddress != nullptr;
        result.push_back(std::move(adapter));
    }
    // Without an explicit adapter, the host's actual connection comes first.
    std::stable_sort(result.begin(), result.end(), [](const HostAdapter& a, const HostAdapter& b) {
        return (a.gateway && a.up) > (b.gateway && b.up);
    });
    return result;
}

} // namespace

struct BridgeNetworkBackend::Api {
    HMODULE library = nullptr;
    void* (*create)(const char*, char*) = nullptr;
    int (*setSnaplen)(void*, int) = nullptr;
    int (*setPromisc)(void*, int) = nullptr;
    int (*setTimeout)(void*, int) = nullptr;
    int (*setImmediateMode)(void*, int) = nullptr;
    int (*setBufferSize)(void*, int) = nullptr;
    int (*activate)(void*) = nullptr;
    int (*setNonBlock)(void*, int, char*) = nullptr;
    int (*nextEx)(void*, PcapPacketHeader**, const unsigned char**) = nullptr;
    int (*sendPacket)(void*, const unsigned char*, int) = nullptr;
    HANDLE (*getEvent)(void*) = nullptr;
    void (*close)(void*) = nullptr;
    char* (*getError)(void*) = nullptr;
    int (*compile)(void*, BpfProgram*, const char*, int, std::uint32_t) = nullptr;
    int (*setFilter)(void*, BpfProgram*) = nullptr;
    void (*freeCode)(BpfProgram*) = nullptr;

    ~Api()
    {
        // wpcap stays loaded: Npcap keeps global state across VM resets.
    }

    bool Load()
    {
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring path = std::wstring(system) + L"\\Npcap\\wpcap.dll";
        // The altered search path finds Packet.dll next to wpcap.dll.
        library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (library == nullptr) return false;
        bool complete = true;
        const auto bind = [&](auto& slot, const char* name) {
            slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(GetProcAddress(library, name));
            complete = complete && slot != nullptr;
        };
        bind(create, "pcap_create");
        bind(setSnaplen, "pcap_set_snaplen");
        bind(setPromisc, "pcap_set_promisc");
        bind(setTimeout, "pcap_set_timeout");
        bind(setImmediateMode, "pcap_set_immediate_mode");
        bind(setBufferSize, "pcap_set_buffer_size");
        bind(activate, "pcap_activate");
        bind(setNonBlock, "pcap_setnonblock");
        bind(nextEx, "pcap_next_ex");
        bind(sendPacket, "pcap_sendpacket");
        bind(getEvent, "pcap_getevent");
        bind(close, "pcap_close");
        bind(getError, "pcap_geterr");
        bind(compile, "pcap_compile");
        bind(setFilter, "pcap_setfilter");
        bind(freeCode, "pcap_freecode");
        return complete;
    }

    void* Open(const std::string& device, bool promiscuous, std::string& error)
    {
        char message[512] = {};
        void* handle = create(device.c_str(), message);
        if (handle == nullptr) {
            error = message;
            return nullptr;
        }
        setSnaplen(handle, static_cast<int>(kMaxFrame));
        setPromisc(handle, promiscuous ? 1 : 0);
        setTimeout(handle, 1);
        setImmediateMode(handle, 1);
        setBufferSize(handle, 4 * 1024 * 1024);
        if (activate(handle) < 0) {
            error = getError(handle);
            close(handle);
            return nullptr;
        }
        return handle;
    }
};

BridgeNetworkBackend::BridgeNetworkBackend(ReceiveFrame receive, const std::string& adapter, const MacAddress& guestMac)
    : receive_(std::move(receive)), api_(std::make_unique<Api>()), guestMac_(guestMac)
{
    if (!api_->Load()) {
        throw std::runtime_error("Npcap n'est pas installé (https://npcap.com) : l'accès par pont en a besoin.");
    }
    const auto adapters = HostAdapters();
    const HostAdapter* chosen = nullptr;
    for (const auto& item : adapters) {
        if (adapter.empty() ? item.up
                            : SameText(adapter, item.friendlyName) || SameText(adapter, item.description) ||
                                  SameText(adapter, item.guid)) {
            chosen = &item;
            break;
        }
    }
    if (chosen == nullptr) {
        throw std::runtime_error(adapter.empty() ? "aucune carte Ethernet ou Wi-Fi connectée pour l'accès par pont."
                                                 : "carte réseau de l'hôte « " + adapter + " » introuvable.");
    }
    adapterName_ = chosen->friendlyName;
    hostMac_ = chosen->mac;
    wireless_ = chosen->wireless;
    if (!chosen->up) VMM_WARN("Accès par pont : la carte « %s » n'est pas connectée.", adapterName_.c_str());

    const std::string device = "\\Device\\NPF_" + chosen->guid;
    std::string error;
    // Promiscuous capture only on wired adapters: Wi-Fi drivers rarely allow it.
    rx_ = api_->Open(device, !wireless_, error);
    if (rx_ != nullptr) tx_ = api_->Open(device, false, error);
    if (rx_ == nullptr || tx_ == nullptr) {
        if (rx_ != nullptr) api_->close(rx_);
        rx_ = nullptr;
        throw std::runtime_error("Npcap ne peut pas ouvrir « " + adapterName_ + " » (" + error +
                                 "). Si Npcap est limité aux administrateurs, réinstalle-le sans cette option.");
    }
    char message[512] = {};
    api_->setNonBlock(rx_, 1, message);
    ApplyFilter();
    transmit_.reserve(2048);
    incoming_.reserve(2048);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (wireless_) {
        VMM_INFO("Accès par pont sur « %s » (Wi-Fi) : la carte %s sort avec l'adresse MAC de l'hôte %s.",
                 adapterName_.c_str(), FormatMac(guestMac_).c_str(), FormatMac(hostMac_).c_str());
    } else {
        VMM_INFO("Accès par pont sur « %s » (Ethernet) : carte %s.", adapterName_.c_str(), FormatMac(guestMac_).c_str());
    }
    thread_ = std::thread([this] { Run(); });
}

BridgeNetworkBackend::~BridgeNetworkBackend()
{
    if (stop_ != nullptr) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (rx_ != nullptr) api_->close(rx_);
    if (tx_ != nullptr) api_->close(tx_);
    if (stop_ != nullptr) CloseHandle(stop_);
}

void BridgeNetworkBackend::ApplyFilter()
{
    // Kernel-side filter: only the frames the guest can want are copied up.
    std::string expression;
    if (!wireless_) {
        expression = "ether dst " + FormatMac(guestMac_) + " or ether multicast";
    } else {
        // DHCP replies too: many routers answer the frame's source MAC (the
        // host's) in unicast, even when asked to broadcast.
        expression = "(ether multicast and not ip6) or arp or (udp and dst port 68)";
        if (const auto ip = guestIp_.load(); ip != 0) expression += " or (ip and dst host " + FormatIp(ip) + ")";
    }
    BpfProgram program{};
    if (api_->compile(rx_, &program, expression.c_str(), 1, 0xFFFFFFFFu) != 0) {
        VMM_WARN("Accès par pont : filtre « %s » refusé (%s).", expression.c_str(), api_->getError(rx_));
        return;
    }
    if (api_->setFilter(rx_, &program) != 0) VMM_WARN("Accès par pont : filtre non appliqué (%s).", api_->getError(rx_));
    api_->freeCode(&program);
}

void BridgeNetworkBackend::LearnGuestIp(std::uint32_t address)
{
    if (address == 0 || address == 0xFFFFFFFFu || guestIp_.exchange(address) == address) return;
    VMM_INFO("Accès par pont : l'invité utilise l'adresse %s.", FormatIp(address).c_str());
    refilter_ = true;
}

bool BridgeNetworkBackend::Send(std::span<const std::uint8_t> frame)
{
    if (frame.size() < 14 || frame.size() > kMaxFrame) return false;
    auto& out = transmit_;
    out.assign(frame.begin(), frame.end());
    if (wireless_) {
        const auto type = Load16(out.data() + 12);
        if (type == kTypeIpv6) return true; // Not bridged over Wi-Fi.
        std::memcpy(out.data() + 6, hostMac_.data(), 6);
        if (type == kTypeArp && out.size() >= 42 && Load16(out.data() + 14) == 1 && Load16(out.data() + 16) == kTypeIpv4 &&
            out[18] == 6 && out[19] == 4) {
            LearnGuestIp(LoadIp(out.data() + 28));
            std::memcpy(out.data() + 22, hostMac_.data(), 6); // Sender hardware address.
        } else if (type == kTypeIpv4 && out.size() >= 34) {
            const std::size_t header = static_cast<std::size_t>(out[14] & 0x0F) * 4;
            LearnGuestIp(LoadIp(out.data() + 26));
            const std::size_t udp = 14 + header;
            // DHCP client -> server: ask for broadcast replies, which reach
            // the guest whatever MAC the access point knows.
            if (out[23] == 17 && out.size() >= udp + 8 + 12 && Load16(out.data() + udp) == 68 &&
                Load16(out.data() + udp + 2) == 67) {
                out[udp + 8 + 10] |= 0x80;
                out[udp + 6] = 0; // UDP checksum is optional over IPv4.
                out[udp + 7] = 0;
                if (!loggedDhcpRequest_) {
                    VMM_INFO("Accès par pont : requête DHCP de l'invité envoyée sur le réseau.");
                    loggedDhcpRequest_ = true;
                }
            }
        }
    }
    if (!loggedSend_) {
        VMM_INFO("Accès par pont : première trame de l'invité (type 0x%04x, %zu octets).", Load16(out.data() + 12), out.size());
        loggedSend_ = true;
    }
    if (api_->sendPacket(tx_, out.data(), static_cast<int>(out.size())) != 0) {
        if (!loggedSendError_) {
            VMM_WARN("Accès par pont : Npcap refuse l'envoi (%s).", api_->getError(tx_));
            if (wireless_) {
                VMM_WARN("Le pilote de la carte Wi-Fi « %s » n'accepte pas l'injection de trames : l'accès par pont "
                         "est impossible sur cette carte. Utilise le NAT (avec redirections de ports) ou une carte "
                         "Ethernet.", adapterName_.c_str());
            }
            loggedSendError_ = true;
        }
        return false;
    }
    return true;
}

namespace {
bool IsDhcpReply(const std::uint8_t* data, std::size_t length)
{
    if (length < 34 || Load16(data + 12) != kTypeIpv4 || data[23] != 17) return false;
    const std::size_t udp = 14 + static_cast<std::size_t>(data[14] & 0x0F) * 4;
    return length >= udp + 8 && Load16(data + udp) == 67 && Load16(data + udp + 2) == 68;
}
} // namespace

void BridgeNetworkBackend::Handle(const std::uint8_t* data, std::size_t length)
{
    if (length < 14 || length > kMaxFrame) return;
    if (!loggedReceive_.exchange(true)) VMM_INFO("Accès par pont : première trame capturée sur la carte de l'hôte.");
    if (IsDhcpReply(data, length) && !loggedDhcpReply_.exchange(true)) {
        VMM_INFO("Accès par pont : réponse DHCP vue sur le réseau (destination %s).",
                 IsMulticast(data) ? "diffusion" : "unicast");
    }
    const std::uint8_t* destination = data;
    const std::uint8_t* source = data + 6;
    const auto type = Load16(data + 12);
    if (!wireless_) {
        if (std::memcmp(source, guestMac_.data(), 6) == 0) return; // Our own injected frame.
        receive_(std::span(data, length));
        return;
    }

    const auto ip = guestIp_.load();
    if (std::memcmp(source, hostMac_.data(), 6) == 0) {
        // Outgoing frames are captured too: drop the guest's own (they carry
        // its IP, or its MAC in DHCP), keep the host's broadcasts. Every ARP
        // frame sent with the host MAC is dropped: the guest's probes (sender
        // IP 0.0.0.0) would otherwise look like an address conflict to it,
        // and the host's own ARP traffic is of no use to the guest.
        if (type == kTypeArp) return;
        if (type == kTypeIpv4 && length >= 34) {
            if (ip != 0 && LoadIp(data + 26) == ip) return;
            const std::size_t udp = 14 + static_cast<std::size_t>(data[14] & 0x0F) * 4;
            if (data[23] == 17 && length >= udp + 8 + 34 && Load16(data + udp) == 68 &&
                std::memcmp(data + udp + 8 + 28, guestMac_.data(), 6) == 0) {
                return;
            }
        }
    }
    if (IsMulticast(destination)) {
        if (type == kTypeIpv6) return;
        receive_(std::span(data, length));
        return;
    }
    if (std::memcmp(destination, hostMac_.data(), 6) != 0) return;
    auto& frame = incoming_;
    if (IsDhcpReply(data, length)) {
        // Unicast DHCP reply: it is the guest's when it names the guest's MAC.
        const std::size_t udp = 14 + static_cast<std::size_t>(data[14] & 0x0F) * 4;
        if (length < udp + 8 + 34 || std::memcmp(data + udp + 8 + 28, guestMac_.data(), 6) != 0) return;
        frame.assign(data, data + length);
        std::memcpy(frame.data(), guestMac_.data(), 6);
        receive_(frame);
        return;
    }
    if (ip == 0) return;
    if (type == kTypeArp && length >= 42 && LoadIp(data + 38) == ip) {
        frame.assign(data, data + length);
        std::memcpy(frame.data(), guestMac_.data(), 6);
        std::memcpy(frame.data() + 32, guestMac_.data(), 6); // Target hardware address.
    } else if (type == kTypeIpv4 && length >= 34 && LoadIp(data + 30) == ip) {
        frame.assign(data, data + length);
        std::memcpy(frame.data(), guestMac_.data(), 6);
    } else {
        return; // The host's own traffic.
    }
    receive_(frame);
}

void BridgeNetworkBackend::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Accès par pont");
    const HANDLE packets = api_->getEvent(rx_);
    bool reported = false;
    for (;;) {
        if (refilter_.exchange(false)) ApplyFilter();
        PcapPacketHeader* header = nullptr;
        const unsigned char* data = nullptr;
        int result = 0;
        for (int budget = 0; budget < 512; ++budget) {
            result = api_->nextEx(rx_, &header, &data);
            if (result != 1) break;
            Handle(data, header->captured);
        }
        if (result < 0 && !reported) {
            VMM_WARN("Accès par pont : lecture impossible sur « %s » (%s).", adapterName_.c_str(), api_->getError(rx_));
            reported = true;
        }
        const HANDLE handles[2] = {stop_, packets};
        if (WaitForMultipleObjects(packets != nullptr ? 2 : 1, handles, FALSE, result < 0 ? 500 : 20) == WAIT_OBJECT_0) {
            break;
        }
    }
}

} // namespace vmm
