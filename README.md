# AZORIA Display Control

一个本地运行的桌面显示器控制项目。AZORIA Desktop 是主控制端，AZORIA Touch
是可选的 480×480 ESP32-S3 实体输入终端。

## 功能

- Electron 桌面控制中心，提供亮度、音量、静音和信号源控制；
- 自动探测 DDC/CI 的承载路径，并按显示器配置表为每项能力选路和回退；
- 在桌面端检测 AZORIA Touch 并完成 Wi‑Fi 配置；
- 设置中提供默认关闭的开发者模式，用于固件刷写和硬件诊断；
- Desktop 主动发现、Touch 被动响应的 Wi‑Fi 局域网通信，以及 BLE 备用连接；
- 固件内置带大小与 SHA-256 校验的 BLE OTA 服务；
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

AZORIA Touch 是可选的实体输入终端。首次使用时通过 USB 点击“准备蓝牙”，随后日常通信
可以只使用 BLE，不要求 Touch 接入 Wi‑Fi。2.4 GHz Wi‑Fi 是可选的局域网连接方式。使用 Wi‑Fi 时，Touch 不主动扫描主机，而是在私有局域网
被动等待；Desktop 主动广播发现请求，校验协议字段和设备标识后向 Touch 下发本机私网地址
和控制端口。BLE 和 Wi‑Fi 都可以独立作为 Touch 到 Desktop 的连接路径。

Touch 通过 Wi‑Fi 或 BLE 建立连接后，由 Desktop 状态响应同步当前 Unix 时间和主机时区
偏移，因此屏幕时间跟随当前 Desktop，不依赖固定时区或单独的公网 NTP 服务。

局域网通信使用三个固定端口：TCP `8732` 提供状态和设备登记，UDP `8733` 用于 Desktop
发现 Touch 及返回命令结果，UDP `8734` 用于 Desktop 心跳、协调和 Touch 控制命令广播。
多个 Desktop 同时在线时，只有实际具备可用 DDC/CI 路径的主机可以执行命令。首次建立
控制关系后，该主机成为粘性 Master；只要心跳和 DDC/CI 路径保持正常，后续命令不会重复
选举。Master 断线、路径失效或执行失败时，其他有能力的 Desktop 才重新竞争执行权；网络
分区恢复后若出现多个 Master，则按稳定的 Desktop ID 消除冲突。同一命令 ID 的重发使用
缓存结果，不会重复写入显示器。

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

AZORIA Desktop 的渲染进程没有 Node.js 权限，硬件操作通过白名单 IPC 完成。Desktop
可以主动发起公网 HTTP/HTTPS 请求，但显示器控制、Touch 发现和协调服务只监听私有 IPv4
地址或 UDP 通配接收地址，并按来源所属的私有子网过滤数据包；公网来源的入站请求会被
拒绝。局域网协议不做身份认证，应仅在可信局域网中使用。

## 显示器配置表

显示器控制协议统一为 DDC/CI。Desktop 会区分两种承载路径：显示器 USB 控制接口
提供的“USB HID → DDC/CI”，以及 HDMI、DisplayPort 或 USB-C 视频连接提供的
“视频链路 → DDC/CI”。程序会实际探测路径，而不是只按型号猜测。

这里的“USB HID”不是另一套显示器控制协议。它只是某些显示器用来承载 DDC/CI
报文的厂商 USB 通道；亮度、音量、静音和输入源最终仍以 DDC/CI VCP 功能进行表达。
常用 VCP opcode 如下。配置表中的数字使用 JSON 十进制写法：

| 能力 | VCP opcode | JSON 十进制 |
| --- | --- | ---: |
| 亮度 | `0x10` | `16` |
| 音量 | `0x62` | `98` |
| 静音 | `0x8D` | `141` |
| 输入源 | `0x60` | `96` |

内置配置表位于 `desktop/profiles/`，描述显示器识别条件、VCP opcode、输入源编码和
每项能力的承载路径优先级。设置页可以加载一份 JSON 配置表；文件经过结构和数值范围
校验后保存到本机配置目录，不允许指定命令、可执行文件或网络地址。验证成熟的配置表
可以直接加入 `desktop/profiles/`，随之后的软件版本内置。

USB HID 报文封装可能因厂商而异，因此配置表只能引用软件内置的命名适配器，不能自行
注入底层报文或程序路径。当前内置 `lg-monitor-controls-v1`；增加其他厂商时先实现并审计
对应适配器，再由配置表完成型号匹配和 VCP 路由。

显示器硬件访问集中在 `sidecar/`。Sidecar 随 Desktop 构建和发布，运行时无需另外安装
DDC 控制工具。

### 配置表字段

配置表是 Desktop 与显示器硬件适配层之间的声明式契约，不包含可执行命令。一个完整配置
由识别条件、能力路由、USB HID 映射和视频链路 DDC/CI 映射组成。

| 字段 | 含义 |
| --- | --- |
| `id` | 配置的稳定标识，只允许小写字母、数字和连字符，长度为 2–64。用户配置与内置配置同名时，以用户配置为准。 |
| `name` | 面向用户显示的型号名称，最长 80 个字符。 |
| `fallback` | 可选。设为 `true` 表示没有匹配到专用型号时使用的通用配置。 |
| `match.displayNamePattern` | 可选、不区分大小写的正则表达式，用于匹配操作系统报告的显示器名称，最长 120 个字符。 |
| `match.usbHid.vendorId` | 可选的 USB Vendor ID，使用十进制整数 `0–65535`。 |
| `match.usbHid.productId` | 可选的 USB Product ID，使用十进制整数 `0–65535`。VID/PID 必须同时匹配。 |
| `routes` | `brightness`、`volume`、`mute`、`input` 四项能力各自的承载路径优先级。每项都必须至少有一条路径。 |
| `usbHid` | 当任一路由包含 `usb-hid-ddc` 时必填，描述已内置 USB HID 适配器及其 VCP 映射。 |
| `ddc` | 当任一路由包含 `video-ddc` 时必填，描述视频链路上的输入源读写编码。 |

Desktop 先探测 USB HID 和视频链路 DDC/CI 是否真正可用，再按 `routes` 从左到右为每项
能力选择路径。运行中某项能力连续读取失败三次后，只对该能力尝试下一条可用路径；例如亮度
回退到视频 DDC/CI 时，输入源仍可以继续走 USB HID。可用路径只有：

- `usb-hid-ddc`：通过显示器的 USB HID 控制接口承载 DDC/CI；
- `video-ddc`：通过 HDMI、DisplayPort 或 USB-C 视频链路承载 DDC/CI。

`usbHid` 对象字段：

| 字段 | 含义 |
| --- | --- |
| `adapter` | USB 报文封装适配器。目前只接受内置的 `lg-monitor-controls-v1`。 |
| `vcp` | 四项能力对应的 8 位 VCP opcode，JSON 中写十进制 `0–255`。 |
| `inputWriteMode` | `vcp` 表示用输入源 VCP 及 `inputValues` 写入；`vendor-private` 表示调用该适配器内置的厂商输入切换命令。 |
| `inputValues` | 该 USB HID 路径返回和写入的输入源编码，键为 `dp1`、`hdmi1`、`hdmi2`、`usbc`，值为整数。`vendor-private` 模式写入时由适配器处理，但读取仍使用此表解码。 |

`ddc` 对象字段：

| 字段 | 含义 |
| --- | --- |
| `inputReadValues` | 将显示器读取到的原始输入源值转换成 Desktop 的逻辑输入源。JSON 对象键是原始数值的十进制字符串。 |
| `inputWriteValues` | 将 `dp1`、`hdmi1`、`hdmi2`、`usbc` 转换成写给显示器的原始值。值使用十进制字符串，写入时转换为整数。 |
| `inputWriteFeature` | 输入源写入编码族：`input` 表示标准输入源编码，`input-alt` 表示厂商替代编码。当前 Rust Sidecar 均通过 VCP `0x60` 写入，实际差异由 `inputWriteValues` 表达。 |

读取值与写入值是两个独立映射。部分显示器在 Get VCP、Set VCP、USB HID 和视频链路上
使用不同编码，甚至会返回超出标准 MCCS 表的值，因此不能把 `inputReadValues` 反转后直接
用于写入。所有非标准值都应从真实硬件的稳定回读和写入测试中取得。

### LG 32UQ85R 配置说明

`desktop/profiles/lg-32uq85r.json` 可以这样理解：

- `displayNamePattern` 匹配系统报告的 `LG … 32UQ85…`；USB `1086:39481` 对应十六进制
  VID/PID `043E:9A39`，任一识别条件匹配即可选择这份专用配置；
- 四项能力都优先使用 `usb-hid-ddc`，探测不可用时回退到 `video-ddc`；
- USB HID 路径使用 `lg-monitor-controls-v1` 封装，`16`、`98`、`141`、`96` 分别是
  VCP `0x10`、`0x62`、`0x8D`、`0x60`；
- 输入切换使用 LG 厂商命令，而 USB 路径的输入源回读值由 `inputValues` 解码；
- 视频 DDC/CI 路径会把读取到的 `15`、`17`、`18`、`27`、`3840` 转换成界面使用的
  输入源名称；写入则采用 `208`、`144`、`145`、`210` 这组经硬件验证的替代编码；
- 同一个原始值在通用配置与专用配置中可能有不同含义，专用配置应以对应型号的真实行为为准。

### 增加显示器型号

1. 复制 `desktop/profiles/generic-ddc.json`，使用新的稳定 `id` 命名文件；
2. 记录操作系统显示器名称，并通过 USB 枚举确认十六进制 VID/PID，再转换成 JSON 十进制；
3. 分别测试亮度、音量、静音、输入源在 USB HID 和视频 DDC/CI 上的读写能力；
4. 为每项能力按可靠性填写 `routes`，不要声明未经测试的路径；
5. 对每个输入源分别记录读取原始值与成功写入值，不要假定二者相同；
6. 在 Desktop 设置中导入配置并查看本地诊断日志，确认写入后的回读值和实际显示器状态一致；
7. 提交内置配置前，应在断开 USB、切换视频口、睡眠唤醒和重启后重新验证回退行为。

配置加载器会拒绝非法 ID、过长名称或正则、未知路径、越界 VID/PID 和 VCP opcode，以及
未内置的 USB HID 适配器。配置文件不能指定程序、命令参数、动态库或网络地址。

## 本地诊断日志

每次显示器参数调整都会写入操作来源、参数、目标值、DDC/CI 承载路径、回读结果、耗时和
失败原因。日志文件为 Electron 系统日志目录下的 `azoria-desktop.jsonl`，单文件最多
2 MB，并保留三份轮转历史。日志不记录 Wi‑Fi 密码、本机密钥、设备 MAC 或完整固件路径，
也不会上传到网络。

## 许可证

本项目按 [GPL-3.0-or-later](LICENSE) 发布。GPL 允许商业使用、修改、分发和销售，
但衍生作品必须遵守 GPL 的相应义务。第三方库、硬件、商标和图标仍受各自权利人约束。
