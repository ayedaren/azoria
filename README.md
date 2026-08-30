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
- Rust Sidecar 提供跨平台原生 DDC/CI 与 LG USB HID/DDC 控制。

## 目录

```text
azoria-display-control/
├── desktop/
│   ├── profiles/        # 内置显示器配置表
│   └── src/
│       ├── main/        # Electron 主进程与硬件协调
│       ├── preload/     # 白名单 IPC 桥
│       ├── renderer/    # React + shadcn/ui 界面
│       └── shared/      # 跨进程类型契约
├── firmware/
│   ├── assets/          # 固件图标源文件
│   ├── include/         # 板卡配置
│   └── src/
│       ├── features/    # AZORIA Touch 产品功能
│       ├── platform/    # 屏幕与触摸硬件适配
│       ├── services/    # 配网、BLE 与设备配置
│       └── ui/          # LVGL 公共界面组件和生成资源
└── sidecar/             # Rust DDC/CI 与 USB HID 硬件层
```

## 硬件

当前针对 VIEWE UEDX48480040E-WB-A V1.3、480×480 GC9503 RGB LCD、FT6336U
触摸屏和 16MB Flash / 8MB PSRAM 验证。显示器控制目前已验证 LG 32UQ85R；其他型号需要单独确认 DDC 或 USB HID 映射。

## 快速开始

开发环境需要 Node.js、Rust 和 PlatformIO。

```bash
npm install
npm run sidecar:build
npm run dev
```

桌面端首次启动时会在应用数据目录生成本机密钥。密钥、Wi‑Fi 密码和设备配置
不会写入源码。主窗口可以控制显示器，并检测通过 USB 连接的小屏幕。

AZORIA Touch 是可选的实体输入终端。普通用户使用已预装固件的设备，通过
“AZORIA Touch”页写入 2.4 GHz Wi‑Fi。Touch 不主动扫描主机，而是在私有局域网
被动等待；Desktop 主动广播发现请求，校验设备签名后向 Touch 下发本机私网地址
和控制端口。局域网不可达时可以使用 BLE 备用连接。

固件开发、恢复或测试时，在 AZORIA Desktop 设置中开启“开发者模式”，再进入
出现的“开发者”页。选择本地 AZORIA Touch `.bin` 固件后，程序会校验镜像类型、
版本、校验和与设备身份，并在实际写入前再次核验文件哈希。

## 构建 AZORIA Touch 固件

在项目根目录执行：

```bash
cd firmware
pio run -e viewe_uedx48480040e_wb_a
```

构建成功后，Desktop 所需的应用固件位于：

```text
firmware/.pio/build/viewe_uedx48480040e_wb_a/firmware.bin
```

在 Desktop 的“开发者”页点击“选择固件”，选择这个 `firmware.bin`。不要选择同目录的
`bootloader.bin` 或 `partitions.bin`；它们不是 Desktop 刷写入口接受的应用镜像。
固件版本来自 `firmware/version.txt`，构建前应先更新版本号。`.pio/` 是本地构建目录，
不会提交到仓库；发布版本应把经过测试的 `firmware.bin` 作为独立发布附件提供。

也可以绕过 Desktop，直接由 PlatformIO 构建并上传当前源码：

```bash
cd firmware
pio run -e viewe_uedx48480040e_wb_a -t upload --upload-port /dev/cu.usbmodemXXXX
```

AZORIA Desktop 的渲染进程没有 Node.js 权限，硬件操作通过白名单 IPC 完成。
运行时不连接云服务、遥测或公网校时服务，并阻止桌面 WebView 发起公网 HTTP/HTTPS 请求。
局域网发现和控制服务只监听私有 IPv4 地址，并限制为同一子网。局域网协议不做身份认证，
应仅在可信局域网中使用。

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

显示器硬件访问集中在 `sidecar/`。Sidecar 随 Desktop 构建和发布，运行时无需另外安装
DDC 控制工具。

## 本地诊断日志

每次显示器参数调整都会写入操作来源、参数、目标值、DDC/CI 承载路径、回读结果、耗时和
失败原因。日志文件为 Electron 系统日志目录下的 `azoria-desktop.jsonl`，单文件最多
2 MB，并保留三份轮转历史。日志不记录 Wi‑Fi 密码、本机密钥、设备 MAC 或完整固件路径，
也不会上传到网络。

## 许可证

本项目按 [GPL-3.0-or-later](LICENSE) 发布。GPL 允许商业使用、修改、分发和销售，
但衍生作品必须遵守 GPL 的相应义务。第三方库、硬件、商标和图标仍受各自权利人约束。
