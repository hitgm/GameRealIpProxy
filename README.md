# GameRealIpProxy - 多 Gate 透明真实 IP 透传网关 (WinDivert + C++17)

本工程专为部署在 Windows Server (2012 R2 / 2019 / 2022) 上的**闭源多 Gate 游戏服务端**设计，支持直连 TCP 和加速节点（如腾讯云 EdgeOne）的 Proxy Protocol v1/v2 TCP4/TCP6；结合 **WinDivert 内核级数据包改写**，让 Gate 在对应网络路径配置正确时读取客户端真实 IP。直连连接使用 TCP 对端地址，加速连接使用 Proxy Protocol 头中的客户端地址。

## 目录结构说明

下载本批文件后，请按如下目录结构存放（需自行从 WinDivert 官网下载 SDK 放入 `third_party/windivert`）：

```text
GameRealIpProxy/
├── CMakeLists.txt                  # CMake 跨版本构建脚本（推荐）
├── GameRealIpProxy.vcxproj         # Visual Studio 2019/2022 工程文件
├── config.ini                      # 多 Gate 端口映射与虚拟 IP 配置文件
├── src/
│   └── main.cpp                    # 完整 C++ 核心源码
└── third_party/
    └── windivert/                  # 解压 WinDivert-2.2.2-A 到此目录
        ├── include/
        │   └── windivert.h
        └── x64/
            ├── WinDivert.lib
            ├── WinDivert.dll
            └── WinDivert64.sys
```

## 快速构建指南

### 方式一：使用 CMake 构建（支持 VS 2019 / 2022 及命令行）
1. 下载官方 [WinDivert 2.2.2-A](https://reqrypt.org/windivert.html) 并解压至 `third_party/windivert/`。
2. 在工程根目录打开终端执行：
   ```cmd
   cmake -B build -A x64
   cmake --build build --config Release
   ```
3. 编译完成后，可执行文件 `GameRealIpProxy.exe`、`config.ini`、`WinDivert.dll` 和 `WinDivert64.sys` 会自动输出到 `build/Release/` 目录。

### 方式二：直接打开 Visual Studio 工程
1. 确保 `main.cpp` 位于 `src/` 子目录下，`WinDivert` 文件位于 `third_party/windivert/`。
2. 双击 `GameRealIpProxy.vcxproj` 打开工程，顶部工具栏选择 **`Release` | `x64`**，按 `Ctrl+Shift+B` 生成解决方案，输出文件位于 `bin/Release/`。

## 服务器部署步骤（必读）

1. **添加虚拟 IP 到本机网卡**：
   - 打开 **控制面板 -> 网络和共享中心 -> 更改适配器设置**。
   - 右键当前内网网卡 -> **属性 -> IPv4 -> 属性 -> 高级**。
   - 在“IP 地址”栏点击 **添加**，输入 `10.254.0.1`，子网掩码 `255.255.0.0`（确定保存）。
2. **修改 `config.ini`**：
   - 将 `TargetGateHost` 修改为当前 Windows 服务器的内网 IP（如 `10.0.0.5`，**不要填 `127.0.0.1`**，否则环回流量不会经过 WinDivert 网络层）。
   - IPv6 客户端要让 Gate 获取真实 IPv6，需在 `[General]` 同时配置 `VirtualIPv6`（已分配给本机的 IPv6 地址）和 `TargetGateHostIPv6`（Gate 可监听/访问的 IPv6 地址）；只配置其中一个会导致启动失败。Gate 必须接受 IPv6 连接。留空则只启用 IPv4。
   - 在 `[PortMappings]` 下按需增减 `接入端口=Gate端口` 映射规则。
3. **以管理员权限运行**：
   - 必须右键 **以管理员身份运行** `GameRealIpProxy.exe`（加载 `WinDivert64.sys` 内核驱动需要管理员权限）。
   - 生产环境建议使用 `NSSM` 将其注册为 Windows 系统服务实现开机自启与崩溃守护。

构建时仅在输出目录尚无 `config.ini` 时复制默认配置；已有配置会保留，不会被工程目录中的模板覆盖。
