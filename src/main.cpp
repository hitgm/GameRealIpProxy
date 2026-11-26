#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <shared_mutex>
#include <atomic>
#include <cstring>
#include <csignal>
#include <array>

#include "windivert.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "WinDivert.lib")

// ==========================================
// 1. 数据结构与全局映射表
// ==========================================
struct IpAddress {
    uint8_t family = 0; // AF_INET or AF_INET6
    std::array<uint8_t, 16> bytes{};
    bool operator==(const IpAddress& other) const { return family == other.family && bytes == other.bytes; }
};

struct IpAddressHash {
    std::size_t operator()(const IpAddress& a) const {
        size_t h = std::hash<uint8_t>()(a.family);
        for (uint8_t b : a.bytes) h = (h * 131) ^ b;
        return h;
    }
};

struct NatKey {
    IpAddress ip;          // 入向为 VirtualIP，出向为真实客户端 IP
    uint16_t proxyPort;   // 网络字节序 Proxy 分配的本地源端口
    uint16_t gatePort;    // 网络字节序 Gate 监听端口

    bool operator==(const NatKey& other) const {
        return ip == other.ip && proxyPort == other.proxyPort && gatePort == other.gatePort;
    }
};

struct NatKeyHash {
    std::size_t operator()(const NatKey& k) const {
        return (IpAddressHash{}(k.ip)) ^
               (std::hash<uint16_t>()(k.proxyPort) << 1) ^
               (std::hash<uint16_t>()(k.gatePort) << 2);
    }
};

struct AppConfig {
    std::string virtualIpStr;
    std::string targetGateHostStr;
    std::string virtualIp6Str;
    std::string targetGateHost6Str;
    IpAddress virtualIp4, targetGateHost4, virtualIp6, targetGateHost6;
    int divertThreads = 2;
    std::unordered_map<uint16_t, uint16_t> portMap; // ListenPort -> GatePort (主机字节序)
    std::unordered_set<uint16_t> gatePortsHost;     // 所有 Gate 端口集合 (主机字节序)
};

AppConfig g_config;
std::shared_mutex g_natMutex;
std::unordered_map<NatKey, IpAddress, NatKeyHash> g_inMap;
std::unordered_map<NatKey, IpAddress, NatKeyHash> g_outMap;
std::atomic<bool> g_running{ true };

void SignalHandler(int) {
    g_running = false;
}

void RegisterNatSession(const IpAddress& virtualIp, const IpAddress& realClientIp, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::unique_lock<std::shared_mutex> lock(g_natMutex);
    g_inMap[{virtualIp, proxyPortNet, gatePortNet}] = realClientIp;
    g_outMap[{realClientIp, proxyPortNet, gatePortNet}] = virtualIp;
}

void UnregisterNatSession(const IpAddress& virtualIp, const IpAddress& realClientIp, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::unique_lock<std::shared_mutex> lock(g_natMutex);
    g_inMap.erase({ virtualIp, proxyPortNet, gatePortNet });
    g_outMap.erase({ realClientIp, proxyPortNet, gatePortNet });
}

IpAddress LookupRealIp(const IpAddress& virtualIp, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::shared_lock<std::shared_mutex> lock(g_natMutex);
    auto it = g_inMap.find({ virtualIp, proxyPortNet, gatePortNet });
    return (it != g_inMap.end()) ? it->second : IpAddress{};
}

IpAddress LookupVirtualIp(const IpAddress& realClientIp, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::shared_lock<std::shared_mutex> lock(g_natMutex);
    auto it = g_outMap.find({ realClientIp, proxyPortNet, gatePortNet });
    return (it != g_outMap.end()) ? it->second : IpAddress{};
}

bool IsValid(const IpAddress& ip) { return ip.family == AF_INET || ip.family == AF_INET6; }

// ==========================================
// 2. 配置文件解析
// ==========================================
bool LoadConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[Error] 无法打开配置文件: " << filename << std::endl;
        return false;
    }

    std::string line, section;
    while (std::getline(file, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, (last - first + 1));

        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;
        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        // 去除 key/val 两端空白
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t") + 1);

        if (section == "General") {
            if (key == "VirtualIP") g_config.virtualIpStr = val;
            else if (key == "TargetGateHost") g_config.targetGateHostStr = val;
            else if (key == "VirtualIPv6") g_config.virtualIp6Str = val;
            else if (key == "TargetGateHostIPv6") g_config.targetGateHost6Str = val;
            else if (key == "DivertThreads") g_config.divertThreads = std::max(1, std::stoi(val));
        }
        else if (section == "PortMappings") {
            uint16_t listenPort = static_cast<uint16_t>(std::stoi(key));
            uint16_t gatePort = static_cast<uint16_t>(std::stoi(val));
            g_config.portMap[listenPort] = gatePort;
            g_config.gatePortsHost.insert(gatePort);
        }
    }

    g_config.virtualIp4.family = AF_INET;
    g_config.targetGateHost4.family = AF_INET;
    if (inet_pton(AF_INET, g_config.virtualIpStr.c_str(), g_config.virtualIp4.bytes.data()) != 1 ||
        inet_pton(AF_INET, g_config.targetGateHostStr.c_str(), g_config.targetGateHost4.bytes.data()) != 1 ||
        g_config.portMap.empty()) {
        std::cerr << "[Error] 配置文件参数无效或端口映射为空！" << std::endl;
        return false;
    }
    if (g_config.virtualIp4 == g_config.targetGateHost4) {
        std::cerr << "[Error] VirtualIP 必须与 TargetGateHost 不同。" << std::endl;
        return false;
    }
    if (!g_config.virtualIp6Str.empty() || !g_config.targetGateHost6Str.empty()) {
        g_config.virtualIp6.family = AF_INET6;
        g_config.targetGateHost6.family = AF_INET6;
        if (g_config.virtualIp6Str.empty() || g_config.targetGateHost6Str.empty() ||
            inet_pton(AF_INET6, g_config.virtualIp6Str.c_str(), g_config.virtualIp6.bytes.data()) != 1 ||
            inet_pton(AF_INET6, g_config.targetGateHost6Str.c_str(), g_config.targetGateHost6.bytes.data()) != 1) {
            std::cerr << "[Error] VirtualIPv6 与 TargetGateHostIPv6 必须同时配置为有效 IPv6 地址。" << std::endl;
            return false;
        }
        if (g_config.virtualIp6 == g_config.targetGateHost6) {
            std::cerr << "[Error] VirtualIPv6 必须与 TargetGateHostIPv6 不同。" << std::endl;
            return false;
        }
    }
    return true;
}

bool SendAll(SOCKET dst, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(dst, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool ReceiveExact(SOCKET s, char* data, int len) {
    int received = 0;
    while (received < len) {
        int n = recv(s, data + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

// EO adds Proxy Protocol to the TCP byte stream after the TCP handshake.
// Read exactly the header so that a fragmented header is never mistaken for payload.
bool ReceiveProxyHeader(SOCKET s, const sockaddr_storage& peerAddr, IpAddress& clientIp) {
    static const uint8_t v2Sig[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D,
                                       0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };
    static const char v1Sig[] = "PROXY ";
    if (peerAddr.ss_family == AF_INET) {
        clientIp.family = AF_INET;
        clientIp.bytes.fill(0);
        memcpy(clientIp.bytes.data(), &reinterpret_cast<const sockaddr_in*>(&peerAddr)->sin_addr, 4);
    } else if (peerAddr.ss_family == AF_INET6) {
        const auto* peer6 = reinterpret_cast<const sockaddr_in6*>(&peerAddr);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(&peer6->sin6_addr);
        static const uint8_t mappedPrefix[12] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF};
        if (memcmp(raw, mappedPrefix, sizeof(mappedPrefix)) == 0) {
            clientIp.family = AF_INET;
            clientIp.bytes.fill(0);
            memcpy(clientIp.bytes.data(), raw + 12, 4);
        } else {
            clientIp.family = AF_INET6;
            memcpy(clientIp.bytes.data(), raw, 16);
        }
    } else return false;

    // A direct TCP client has no Proxy Protocol preamble. Peek without consuming
    // payload so the original game bytes can be relayed unchanged.
    bool isV1 = false, isV2 = false;
    ULONGLONG deadline = GetTickCount64() + 500;
    for (;;) {
        uint8_t peek[12]{};
        int n = recv(s, reinterpret_cast<char*>(peek), sizeof(peek), MSG_PEEK);
        if (n <= 0) return false;
        const size_t available = static_cast<size_t>(n);
        const size_t v1Cmp = (std::min)(available, sizeof(v1Sig) - 1);
        const size_t v2Cmp = (std::min)(available, sizeof(v2Sig));
        const bool v1Prefix = memcmp(peek, v1Sig, v1Cmp) == 0;
        const bool v2Prefix = memcmp(peek, v2Sig, v2Cmp) == 0;
        if (available >= sizeof(v1Sig) - 1 && v1Prefix) { isV1 = true; break; }
        if (available >= sizeof(v2Sig) && v2Prefix) { isV2 = true; break; }
        if (!v1Prefix && !v2Prefix) return true;
        if (GetTickCount64() >= deadline) return true;
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s, &readSet);
        timeval waitFor{ 0, 50000 };
        if (select(0, &readSet, nullptr, nullptr, &waitFor) == SOCKET_ERROR) return false;
    }

    std::array<uint8_t, 16 + 65535> header{};
    if (!ReceiveExact(s, reinterpret_cast<char*>(header.data()), 1)) return false;

    if (isV1) {
        if (!ReceiveExact(s, reinterpret_cast<char*>(header.data() + 1), 5) ||
            memcmp(header.data(), "PROXY ", 6) != 0) return false;
        size_t used = 6;
        while (used < 108) {
            if (!ReceiveExact(s, reinterpret_cast<char*>(header.data() + used), 1)) return false;
            ++used;
            if (used >= 2 && header[used - 2] == '\r' && header[used - 1] == '\n') break;
        }
        if (used < 2 || header[used - 2] != '\r' || header[used - 1] != '\n') return false;
        std::istringstream iss(std::string(reinterpret_cast<char*>(header.data()), used - 2));
        std::string marker, family, srcIp, dstIp, srcPort, dstPort;
        iss >> marker >> family >> srcIp >> dstIp >> srcPort >> dstPort;
        if (marker != "PROXY" || srcIp.empty()) return false;
        if (family == "TCP4") {
            clientIp.family = AF_INET;
            if (inet_pton(AF_INET, srcIp.c_str(), clientIp.bytes.data()) != 1) return false;
        } else if (family == "TCP6") {
            clientIp.family = AF_INET6;
            if (inet_pton(AF_INET6, srcIp.c_str(), clientIp.bytes.data()) != 1) return false;
        } else return false;
        return true;
    }

    if (!isV2 || header[0] != v2Sig[0] ||
        !ReceiveExact(s, reinterpret_cast<char*>(header.data() + 1), 15) ||
        memcmp(header.data(), v2Sig, sizeof(v2Sig)) != 0) return false;
    const uint8_t verCmd = header[12];
    const uint8_t famProto = header[13];
    uint16_t addrLenNet = 0;
    memcpy(&addrLenNet, header.data() + 14, sizeof(addrLenNet));
    const uint16_t addrLen = ntohs(addrLenNet);
    if ((verCmd & 0xF0) != 0x20 || (verCmd & 0x0F) != 0x01) return false;
    size_t addressBytes = 0;
    if (famProto == 0x11 && addrLen >= 12) { // TCP over IPv4
        clientIp.family = AF_INET;
        addressBytes = 4;
    } else if (famProto == 0x21 && addrLen >= 36) { // TCP over IPv6
        clientIp.family = AF_INET6;
        addressBytes = 16;
    } else return false;
    if (!ReceiveExact(s, reinterpret_cast<char*>(header.data() + 16), addrLen)) return false;
    memcpy(clientIp.bytes.data(), header.data() + 16, addressBytes);
    return true;
}

void PipeData(SOCKET src, SOCKET dst) {
    char buffer[8192];
    while (g_running) {
        int n = recv(src, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        if (!SendAll(dst, buffer, n)) {
            shutdown(dst, SD_BOTH);
            return;
        }
    }
    shutdown(dst, SD_SEND);
}

void HandleClientConnection(SOCKET clientSock, sockaddr_storage peerAddr, uint16_t targetGatePortHost) {
    IpAddress realClientIp;
    if (!ReceiveProxyHeader(clientSock, peerAddr, realClientIp)) {
        std::cerr << "[Warn] Proxy Protocol 头无效或连接提前关闭，拒绝连接。" << std::endl;
        closesocket(clientSock);
        return;
    }

    const IpAddress& virtualIp = realClientIp.family == AF_INET ? g_config.virtualIp4 : g_config.virtualIp6;
    const IpAddress& gateHost = realClientIp.family == AF_INET ? g_config.targetGateHost4 : g_config.targetGateHost6;
    if (!IsValid(virtualIp) || !IsValid(gateHost)) {
        std::cerr << "[Warn] 收到 IPv6 客户端，但未配置 VirtualIPv6/TargetGateHostIPv6。" << std::endl;
        closesocket(clientSock);
        return;
    }

    SOCKET gateSock = socket(realClientIp.family, SOCK_STREAM, IPPROTO_TCP);
    if (gateSock == INVALID_SOCKET) {
        closesocket(clientSock);
        return;
    }

    BOOL nodelay = TRUE;
    setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    setsockopt(gateSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    sockaddr_storage localBindAddr{};
    int localBindLen = 0;
    sockaddr_storage gateTargetAddr{};
    int gateTargetLen = 0;
    if (realClientIp.family == AF_INET) {
        auto* local4 = reinterpret_cast<sockaddr_in*>(&localBindAddr);
        local4->sin_family = AF_INET;
        memcpy(&local4->sin_addr, virtualIp.bytes.data(), 4);
        local4->sin_port = 0;
        localBindLen = sizeof(sockaddr_in);
        auto* target4 = reinterpret_cast<sockaddr_in*>(&gateTargetAddr);
        target4->sin_family = AF_INET;
        memcpy(&target4->sin_addr, gateHost.bytes.data(), 4);
        target4->sin_port = htons(targetGatePortHost);
        gateTargetLen = sizeof(sockaddr_in);
    } else {
        auto* local6 = reinterpret_cast<sockaddr_in6*>(&localBindAddr);
        local6->sin6_family = AF_INET6;
        memcpy(&local6->sin6_addr, virtualIp.bytes.data(), 16);
        local6->sin6_port = 0;
        localBindLen = sizeof(sockaddr_in6);
        auto* target6 = reinterpret_cast<sockaddr_in6*>(&gateTargetAddr);
        target6->sin6_family = AF_INET6;
        memcpy(&target6->sin6_addr, gateHost.bytes.data(), 16);
        target6->sin6_port = htons(targetGatePortHost);
        gateTargetLen = sizeof(sockaddr_in6);
    }

    if (bind(gateSock, reinterpret_cast<sockaddr*>(&localBindAddr), localBindLen) == SOCKET_ERROR) {
        std::cerr << "[Error] 绑定 VirtualIP 失败，请确认网卡已配置 IP: " << g_config.virtualIpStr
                  << " 错误码: " << WSAGetLastError() << std::endl;
        closesocket(gateSock);
        closesocket(clientSock);
        return;
    }

    sockaddr_storage assignedAddr{};
    int addrLen = sizeof(assignedAddr);
    getsockname(gateSock, reinterpret_cast<sockaddr*>(&assignedAddr), &addrLen);
    uint16_t proxyPortNet = realClientIp.family == AF_INET
        ? reinterpret_cast<sockaddr_in*>(&assignedAddr)->sin_port
        : reinterpret_cast<sockaddr_in6*>(&assignedAddr)->sin6_port;
    uint16_t gatePortNet = htons(targetGatePortHost);

    RegisterNatSession(virtualIp, realClientIp, proxyPortNet, gatePortNet);

    if (connect(gateSock, reinterpret_cast<sockaddr*>(&gateTargetAddr), gateTargetLen) == SOCKET_ERROR) {
        UnregisterNatSession(virtualIp, realClientIp, proxyPortNet, gatePortNet);
        closesocket(gateSock);
        closesocket(clientSock);
        return;
    }

    std::thread t1(PipeData, clientSock, gateSock);
    PipeData(gateSock, clientSock);
    shutdown(clientSock, SD_BOTH);
    shutdown(gateSock, SD_BOTH);
    t1.join();

    closesocket(gateSock);
    closesocket(clientSock);
    UnregisterNatSession(virtualIp, realClientIp, proxyPortNet, gatePortNet);
}

// ==========================================
// 5. WinDivert 内核数据包改写引擎
// ==========================================
void WinDivertWorker(int workerId) {
    std::ostringstream filterOss;
    filterOss << "tcp and (((ip.SrcAddr == " << g_config.virtualIpStr;
    if (!g_config.virtualIp6Str.empty()) filterOss << " or ipv6.SrcAddr == " << g_config.virtualIp6Str;
    filterOss << ") and (";
    bool first = true;
    for (uint16_t gp : g_config.gatePortsHost) {
        if (!first) filterOss << " or ";
        filterOss << "tcp.DstPort == " << gp;
        first = false;
    }
    filterOss << ")) or (";
    first = true;
    for (uint16_t gp : g_config.gatePortsHost) {
        if (!first) filterOss << " or ";
        filterOss << "tcp.SrcPort == " << gp;
        first = false;
    }
    filterOss << "))";

    std::string filterStr = filterOss.str();
    if (workerId == 0) {
        std::cout << "[WinDivert] 启用过滤规则: " << filterStr << std::endl;
    }

    HANDLE handle = WinDivertOpen(filterStr.c_str(), WINDIVERT_LAYER_NETWORK, 0, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        std::cerr << "[Fatal] WinDivertOpen 失败! 请以【管理员权限】运行，并确保 WinDivert64.sys 在同级目录。错误码: "
                  << GetLastError() << std::endl;
        g_running = false;
        return;
    }

    std::vector<uint8_t> packet(65535);
    UINT packetLen = 0;
    WINDIVERT_ADDRESS addr{};

    while (g_running) {
        if (!WinDivertRecv(handle, packet.data(), static_cast<UINT>(packet.size()), &packetLen, &addr)) {
            continue;
        }

        PWINDIVERT_IPHDR ipHdr = nullptr;
        PWINDIVERT_IPV6HDR ipv6Hdr = nullptr;
        PWINDIVERT_TCPHDR tcpHdr = nullptr;
        WinDivertHelperParsePacket(packet.data(), packetLen, &ipHdr, &ipv6Hdr, nullptr, nullptr, nullptr,
                                   &tcpHdr, nullptr, nullptr, nullptr, nullptr, nullptr);

        if ((ipHdr != nullptr || ipv6Hdr != nullptr) && tcpHdr != nullptr) {
            uint16_t dstPortHost = ntohs(tcpHdr->DstPort);
            uint16_t srcPortHost = ntohs(tcpHdr->SrcPort);
            IpAddress srcIp{}, dstIp{};
            if (ipHdr != nullptr) {
                srcIp.family = dstIp.family = AF_INET;
                memcpy(srcIp.bytes.data(), &ipHdr->SrcAddr, 4);
                memcpy(dstIp.bytes.data(), &ipHdr->DstAddr, 4);
            } else {
                srcIp.family = dstIp.family = AF_INET6;
                memcpy(srcIp.bytes.data(), ipv6Hdr->SrcAddr, 16);
                memcpy(dstIp.bytes.data(), ipv6Hdr->DstAddr, 16);
            }

            // 入向：代理(VirtualIP) -> Gate 端口
            if (srcIp == (srcIp.family == AF_INET ? g_config.virtualIp4 : g_config.virtualIp6) &&
                g_config.gatePortsHost.count(dstPortHost)) {
                IpAddress realIp = LookupRealIp(srcIp, tcpHdr->SrcPort, tcpHdr->DstPort);
                if (IsValid(realIp)) {
                    if (ipHdr != nullptr) memcpy(&ipHdr->SrcAddr, realIp.bytes.data(), 4);
                    else memcpy(ipv6Hdr->SrcAddr, realIp.bytes.data(), 16);
                    addr.Outbound = 0;
                    WinDivertHelperCalcChecksums(packet.data(), packetLen, &addr, 0);
                }
            }
            // 出向：Gate 端口 -> 玩家真实 IP
            else if (g_config.gatePortsHost.count(srcPortHost)) {
                IpAddress returnVirtualIp = LookupVirtualIp(dstIp, tcpHdr->DstPort, tcpHdr->SrcPort);
                if (IsValid(returnVirtualIp)) {
                    if (ipHdr != nullptr) memcpy(&ipHdr->DstAddr, returnVirtualIp.bytes.data(), 4);
                    else memcpy(ipv6Hdr->DstAddr, returnVirtualIp.bytes.data(), 16);
                    addr.Outbound = 0;
                    WinDivertHelperCalcChecksums(packet.data(), packetLen, &addr, 0);
                }
            }
        }

        WinDivertSend(handle, packet.data(), packetLen, nullptr, &addr);
    }

    WinDivertClose(handle);
}

// ==========================================
// 6. 端口监听器与主入口
// ==========================================
void StartListener(uint16_t listenPortHost, uint16_t gatePortHost) {
    SOCKET listenSock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "[Error] 创建双栈监听套接字失败: " << listenPortHost
                  << " 错误码: " << WSAGetLastError() << std::endl;
        return;
    }
    DWORD v6Only = FALSE;
    setsockopt(listenSock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6Only), sizeof(v6Only));
    sockaddr_in6 serverAddr{};
    serverAddr.sin6_family = AF_INET6;
    serverAddr.sin6_addr = in6addr_any;
    serverAddr.sin6_port = htons(listenPortHost);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR ||
        listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[Error] 监听端口失败: " << listenPortHost << " 错误码: " << WSAGetLastError() << std::endl;
        return;
    }

    std::cout << "[Proxy] 正在监听加速端口 " << listenPortHost << " -> 转发至 Gate 端口 " << gatePortHost << std::endl;

    while (g_running) {
        sockaddr_storage peerAddr{};
        int peerLen = sizeof(peerAddr);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
        if (clientSock != INVALID_SOCKET) {
            std::thread(HandleClientConnection, clientSock, peerAddr, gatePortHost).detach();
        }
    }
    closesocket(listenSock);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::signal(SIGINT, SignalHandler);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Fatal] WSAStartup 失败!" << std::endl;
        return 1;
    }

    if (!LoadConfig("config.ini")) {
        return 1;
    }

    std::vector<std::thread> divertThreads;
    for (int i = 0; i < g_config.divertThreads; ++i) {
        divertThreads.emplace_back(WinDivertWorker, i);
    }

    std::vector<std::thread> listeners;
    for (const auto& kv : g_config.portMap) {
        listeners.emplace_back(StartListener, kv.first, kv.second);
    }

    std::cout << "[System] 多 Gate 真实 IP 透明透传网关已启动！按 Ctrl+C 退出。" << std::endl;

    for (auto& t : listeners) {
        if (t.joinable()) t.join();
    }
    g_running = false;
    for (auto& t : divertThreads) {
        if (t.joinable()) t.join();
    }

    WSACleanup();
    return 0;
}
