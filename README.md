# AZORIA Display Control

一个本地运行的桌面显示器控制项目。AZORIA Desktop 是主控制端，AZORIA Touch
是可选的 480×480 ESP32-S3 实体输入终端。

## 功能

- Electron 桌面控制中心，提供亮度、音量、静音和信号源控制；
- 自动探测 DDC/CI 的承载路径，并按显示器配置表为每项能力选路和回退；
- 在桌面端检测 AZORIA Touch 并完成 Wi‑Fi 配置；
- 设置中提供默认关闭的开发者模式，用于固件刷写和硬件诊断；
- Desktop 主动发现、Touch 被动响应的 Wi‑Fi 局域网通信，以及 BLE 备用连接；
- BLE OTA；
- LG USB HID/DDC 控制；
- 仅保留显示器控制相关 API。

## 目录

```text
azoria-display-control/
├── desktop/             # Electron、TypeScript、React 和 shadcn/ui 桌面端
├── backend/             # 兼容后台与 LG HID 工具
├── firmware/            # ESP32-S3 + LVGL 固件
└── tools/               # OTA 和构建辅助工具
```

## 硬件

当前针对 VIEWE UEDX48480040E-WB-A V1.3、480×480 GC9503 RGB LCD、FT6336U
触摸屏和 16MB Flash / 8MB PSRAM 验证。显示器控制目前已验证 LG 32UQ85R；其他型号需要单独确认 DDC 或 USB HID 映射。

## 快速开始

开发环境需要 macOS 13+、Node.js、PlatformIO 和 `m1ddc`。

```bash
brew install m1ddc
npm install
npm run dev
```

桌面端首次启动时会在应用数据目录生成本机密钥。密钥、Wi‑Fi 密码和设备配置
不会写入源码。主窗口可以控制显示器，并检测通过 USB 连接的小屏幕。

AZORIA Touch 是可选的实体输入终端。普通用户使用已预装固件的设备，通过
“AZORIA Touch”页写入 2.4 GHz Wi‑Fi。Touch 不主动扫描主机，而是在私有局域网
被动等待；Desktop 主动广播发现请求，校验设备签名后向 Touch 下发本机私网地址
和控制端口。局域网不可达时可以使用 BLE 备用连接。

固件开发、恢复或测试时，在 AZORIA Desktop 设置中开启“开发者模式”，再进入
出现的“开发者”页。程序只列出通过厂商与 MAC 初筛的设备，并在写入前再次核验
ESP32-S3 型号和芯片 MAC。

构建固件：

```bash
cd firmware
pio run
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

AZORIA Desktop 的渲染进程没有 Node.js 权限，硬件操作通过白名单 IPC 完成。
运行时不连接云服务、遥测或公网校时服务，并阻止桌面 WebView 发起公网 HTTP/HTTPS 请求。
局域网发现消息和主机配置都使用设备共享密钥签名；密钥不通过局域网传输，控制服务
只监听本机私有 IPv4 地址并要求 Bearer Token。

## 显示器配置表

显示器控制协议统一为 DDC/CI。Desktop 会区分两种承载路径：显示器 USB 控制接口
提供的“USB HID → DDC/CI”，以及 HDMI、DisplayPort 或 USB-C 视频连接提供的
“视频链路 → DDC/CI”。程序会实际探测路径，而不是只按型号猜测。

内置配置表位于 `desktop/profiles/`，描述显示器识别条件、VCP opcode、输入源编码和
每项能力的承载路径优先级。设置页可以加载一份 JSON 配置表；文件经过结构和数值范围
校验后保存到本机配置目录，不允许指定命令、可执行文件或网络地址。验证成熟的配置表
可以直接加入 `desktop/profiles/`，随之后的软件版本内置。

USB HID 报文封装可能因厂商而异，因此配置表只能引用软件内置的命名适配器，不能自行
注入底层报文或程序路径。当前内置 `lg-monitor-controls-v1`；增加其他厂商时先实现并审计
对应适配器，再由配置表完成型号匹配和 VCP 路由。

## 许可证

本项目按 [GPL-3.0-or-later](LICENSE) 发布。GPL 允许商业使用、修改、分发和销售，
但衍生作品必须遵守 GPL 的相应义务。第三方库、硬件、商标和图标仍受各自权利人约束。
