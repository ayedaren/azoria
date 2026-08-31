# Changelog

## Unreleased

- 提供亮度、音量、静音、输入源、Touch 配网和 BLE OTA 固件服务；
- 使用 Electron 控制端与 Rust Sidecar 统一原生 DDC/CI 和 LG USB HID/DDC 路径；
- 支持 Desktop 主动发现 Touch，以及基于 DDC/CI 可达性的粘性 Master 协调；
- 修复 macOS 上绑定单个网卡地址时无法接收 Touch 子网广播的问题；
- 增加固件选择与校验、统一控制链路的显示器配置表、参数写入回读和本地诊断日志。
