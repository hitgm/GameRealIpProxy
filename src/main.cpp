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

#include "windivert.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "WinDivert.lib")

// ==========================================
// 1. 数据结构与全局映射表
// ==========================================
struct NatKey {
    uint32_t ip;          // 网络字节序 IP (入向为 VirtualIP，出向为 RealClientIP)
    uint16_t proxyPort;   // 网络字节序 Proxy 分配的本地源端口
    uint16_t gatePort;    // 网络字节序 Gate 监听端口

    bool operator==(const NatKey& other) const {
        return ip == other.ip && proxyPort == other.proxyPort && gatePort == other.gatePort;
    }
};

struct NatKeyHash {
    std::size_t operator()(const NatKey& k) const {
        return (std::hash<uint32_t>()(k.ip)) ^
               (std::hash<uint16_t>()(k.proxyPort) << 1) ^
               (std::hash<uint16_t>()(k.gatePort) << 2);
    }
};

struct AppConfig {
    std::string virtualIpStr;
    std::string targetGateHostStr;
    uint32_t virtualIpNet = 0;
    uint32_t targetGateHostNet = 0;
    int divertThreads = 2;
    std::unordered_map<uint16_t, uint16_t> portMap; // ListenPort -> GatePort (主机字节序)
    std::unordered_set<uint16_t> gatePortsHost;     // 所有 Gate 端口集合 (主机字节序)
};

AppConfig g_config;
std::shared_mutex g_natMutex;
std::unordered_map<NatKey, uint32_t, NatKeyHash> g_inMap;  // (VirtualIP, ProxyPort, GatePort) -> RealClientIP
std::unordered_map<NatKey, uint32_t, NatKeyHash> g_outMap; // (RealClientIP, ProxyPort, GatePort) -> VirtualIP
std::atomic<bool> g_running{ true };

void SignalHandler(int) {
    g_running = false;
}

void RegisterNatSession(uint32_t virtualIpNet, uint32_t realClientIpNet, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::unique_lock<std::shared_mutex> lock(g_natMutex);
    g_inMap[{virtualIpNet, proxyPortNet, gatePortNet}] = realClientIpNet;
    g_outMap[{realClientIpNet, proxyPortNet, gatePortNet}] = virtualIpNet;
}

void UnregisterNatSession(uint32_t virtualIpNet, uint32_t realClientIpNet, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::unique_lock<std::shared_mutex> lock(g_natMutex);
    g_inMap.erase({ virtualIpNet, proxyPortNet, gatePortNet });
    g_outMap.erase({ realClientIpNet, proxyPortNet, gatePortNet });
}

uint32_t LookupRealIp(uint32_t virtualIpNet, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::shared_lock<std::shared_mutex> lock(g_natMutex);
    auto it = g_inMap.find({ virtualIpNet, proxyPortNet, gatePortNet });
    return (it != g_inMap.end()) ? it->second : 0;
}

uint32_t LookupVirtualIp(uint32_t realClientIpNet, uint16_t proxyPortNet, uint16_t gatePortNet) {
    std::shared_lock<std::shared_mutex> lock(g_natMutex);
    auto it = g_outMap.find({ realClientIpNet, proxyPortNet, gatePortNet });
    return (it != g_outMap.end()) ? it->second : 0;
}

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
            else if (key == "DivertThreads") g_config.divertThreads = std::max(1, std::stoi(val));
        }
        else if (section == "PortMappings") {
            uint16_t listenPort = static_cast<uint16_t>(std::stoi(key));
            uint16_t gatePort = static_cast<uint16_t>(std::stoi(val));
            g_config.portMap[listenPort] = gatePort;
            g_config.gatePortsHost.insert(gatePort);
        }
    }

    if (inet_pton(AF_INET, g_config.virtualIpStr.c_str(), &g_config.virtualIpNet) != 1 ||
        inet_pton(AF_INET, g_config.targetGateHostStr.c_str(), &g_config.targetGateHostNet) != 1 ||
        g_config.portMap.empty()) {
        std::cerr << "[Error] 配置文件参数无效或端口映射为空！" << std::endl;
        return false;
    }
    return true;
}

// ==========================================
// 3. Proxy Protocol v1 & v2 解析器
// ==========================================
int ParseProxyProtocol(const char* buf, int len, uint32_t& outClientIpNet) {
    static const uint8_t v2Sig[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };
    if (len >= 16 && memcmp(buf, v2Sig, 12) == 0) {
        uint8_t verCmd = static_cast<uint8_t>(buf[12]);
        uint8_t fam = static_cast<uint8_t>(buf[13]);
        uint16_t addrLen = ntohs(*reinterpret_cast<const uint16_t*>(buf + 14));
        int totalHeaderLen = 16 + addrLen;
        if (len < totalHeaderLen) return -1;

        if ((verCmd & 0x0F) == 0x01 && fam == 0x11 && addrLen >= 12) { // PROXY + TCP4
            memcpy(&outClientIpNet, buf + 16, 4);
            return totalHeaderLen;
        }
        return totalHeaderLen;
    }

    if (len >= 6 && strncmp(buf, "PROXY ", 6) == 0) {
        const char* crlf = nullptr;
        for (int i = 6; i < len - 1; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n') {
                crlf = buf + i;
                break;
            }
        }
        if (!crlf) return -1;

        std::string headerLine(buf, crlf - buf);
        std::istringstream iss(headerLine);
        std::string proto, inetType, srcIpStr;
        iss >> proto >> inetType >> srcIpStr;
        if (inetType == "TCP4") {
            if (inet_pton(AF_INET, srcIpStr.c_str(), &outClientIpNet) != 1) return -1;
        }
        return static_cast<int>((crlf - buf) + 2);
    }

    return 0;
}

// ==========================================
// 4. 单个客户端连接的双向流转发
// ==========================================
void PipeData(SOCKET src, SOCKET dst) {
    char buffer[8192];
    while (g_running) {
        int n = recv(src, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        int sent = 0;
        while (sent < n) {
            int s = send(dst, buffer + sent, n - sent, 0);
            if (s <= 0) {
                shutdown(dst, SD_BOTH);
                return;
            }
            sent += s;
        }
    }
    shutdown(dst, SD_SEND);
}

void HandleClientConnection(SOCKET clientSock, sockaddr_in clientAddr, uint16_t targetGatePortHost) {
    char initBuf[4096];
    int n = recv(clientSock, initBuf, sizeof(initBuf), 0);
    if (n <= 0) {
        closesocket(clientSock);
        return;
    }

    uint32_t realClientIpNet = clientAddr.sin_addr.s_addr;
    int headerBytes = ParseProxyProtocol(initBuf, n, realClientIpNet);
    if (headerBytes < 0) {
        closesocket(clientSock);
        return;
    }

    SOCKET gateSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (gateSock == INVALID_SOCKET) {
        closesocket(clientSock);
        return;
    }

    BOOL nodelay = TRUE;
    setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    setsockopt(gateSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    sockaddr_in localBindAddr{};
    localBindAddr.sin_family = AF_INET;
    localBindAddr.sin_addr.s_addr = g_config.virtualIpNet;
    localBindAddr.sin_port = 0;

    if (bind(gateSock, (sockaddr*)&localBindAddr, sizeof(localBindAddr)) == SOCKET_ERROR) {
        std::cerr << "[Error] 绑定 VirtualIP 失败，请确认网卡已配置 IP: " << g_config.virtualIpStr
                  << " 错误码: " << WSAGetLastError() << std::endl;
        closesocket(gateSock);
        closesocket(clientSock);
        return;
    }

    sockaddr_in assignedAddr{};
    int addrLen = sizeof(assignedAddr);
    getsockname(gateSock, (sockaddr*)&assignedAddr, &addrLen);
    uint16_t proxyPortNet = assignedAddr.sin_port;
    uint16_t gatePortNet = htons(targetGatePortHost);

    RegisterNatSession(g_config.virtualIpNet, realClientIpNet, proxyPortNet, gatePortNet);

    sockaddr_in gateTargetAddr{};
    gateTargetAddr.sin_family = AF_INET;
    gateTargetAddr.sin_addr.s_addr = g_config.targetGateHostNet;
    gateTargetAddr.sin_port = gatePortNet;

    if (connect(gateSock, (sockaddr*)&gateTargetAddr, sizeof(gateTargetAddr)) == SOCKET_ERROR) {
        UnregisterNatSession(g_config.virtualIpNet, realClientIpNet, proxyPortNet, gatePortNet);
        closesocket(gateSock);
        closesocket(clientSock);
        return;
    }

    int payloadLen = n - headerBytes;
    if (payloadLen > 0) {
        send(gateSock, initBuf + headerBytes, payloadLen, 0);
    }

    std::thread t1(PipeData, clientSock, gateSock);
    PipeData(gateSock, clientSock);
    t1.join();

    closesocket(gateSock);
    closesocket(clientSock);
    Sleep(50);
    UnregisterNatSession(g_config.virtualIpNet, realClientIpNet, proxyPortNet, gatePortNet);
}

// ==========================================
// 5. WinDivert 内核数据包改写引擎
// ==========================================
void WinDivertWorker(int workerId) {
    std::ostringstream filterOss;
    filterOss << "tcp and ((ip.SrcAddr == " << g_config.virtualIpStr << " and (";
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
        PWINDIVERT_TCPHDR tcpHdr = nullptr;
        WinDivertHelperParsePacket(packet.data(), packetLen, &ipHdr, nullptr, nullptr, nullptr, nullptr,
                                   &tcpHdr, nullptr, nullptr, nullptr, nullptr, nullptr);

        if (ipHdr != nullptr && tcpHdr != nullptr) {
            uint16_t dstPortHost = ntohs(tcpHdr->DstPort);
            uint16_t srcPortHost = ntohs(tcpHdr->SrcPort);

            // 入向：代理(VirtualIP) -> Gate 端口
            if (ipHdr->SrcAddr == g_config.virtualIpNet && g_config.gatePortsHost.count(dstPortHost)) {
                uint32_t realIp = LookupRealIp(ipHdr->SrcAddr, tcpHdr->SrcPort, tcpHdr->DstPort);
                if (realIp != 0) {
                    ipHdr->SrcAddr = realIp;
                    addr.Outbound = 0;
                    WinDivertHelperCalcChecksums(packet.data(), packetLen, &addr, 0);
                }
            }
            // 出向：Gate 端口 -> 玩家真实 IP
            else if (g_config.gatePortsHost.count(srcPortHost)) {
                uint32_t virtualIp = LookupVirtualIp(ipHdr->DstAddr, tcpHdr->DstPort, tcpHdr->SrcPort);
                if (virtualIp != 0) {
                    ipHdr->DstAddr = virtualIp;
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
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(listenPortHost);

    if (bind(listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR ||
        listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[Error] 监听端口失败: " << listenPortHost << " 错误码: " << WSAGetLastError() << std::endl;
        return;
    }

    std::cout << "[Proxy] 正在监听加速端口 " << listenPortHost << " -> 转发至 Gate 端口 " << gatePortHost << std::endl;

    while (g_running) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock != INVALID_SOCKET) {
            std::thread(HandleClientConnection, clientSock, clientAddr, gatePortHost).detach();
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
