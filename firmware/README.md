# Firmware

AZORIA Touch 的 ESP32-S3 + LVGL 固件，提供亮度、音量、静音和输入源控制。

## 构建

```bash
pio run -e viewe_uedx48480040e_wb_a
pio run -e viewe_uedx48480040e_wb_a -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200
```

应用固件输出到 `.pio/build/viewe_uedx48480040e_wb_a/firmware.bin`。

## 目录

- `assets/icons/`：生成 LVGL 图标所使用的 SVG 源文件；
- `include/`：VIEWE 板卡配置；
- `src/features/display_control/`：显示器控制状态、命令与触控界面；
- `src/platform/`：LCD、背光和触摸硬件适配；
- `src/services/`：USB 配网、BLE 传输和本机配置；
- `src/ui/`：公共 LVGL 组件、字体和生成后的图标数据。

普通用户使用已预装固件的设备，在 AZORIA Desktop 的“AZORIA Touch”页完成
2.4 GHz Wi‑Fi 配网。开发者需要刷写或恢复固件时，先在桌面端设置中开启
“开发者模式”。BLE OTA 会校验固件大小与 SHA-256 后再重启。

连入 Wi‑Fi 后，Touch 在 UDP 8733 被动等待同一私网内的 Desktop 发现请求，随后接收
Desktop 地址和控制端口。控制操作广播到 UDP 8734，由当前具备 DDC/CI 执行能力的
Desktop Master 消费并在 UDP 8733 返回结果。Master 在连接正常期间保持不变，只有断线、
DDC/CI 路径失效或执行失败时才重新选择。Touch 会用相同命令 ID 重试未确认的最终操作，
Desktop 返回缓存结果以避免重复写入。Touch 不主动扫描局域网主机，也不会连接公网地址。
局域网协议不做身份认证，应仅在可信局域网中使用。

当前硬件验证：VIEWE UEDX48480040E-WB-A V1.3、480×480 GC9503、FT6336U、16MB
Flash 和 8MB PSRAM。

代码遵循根目录 GPL-3.0-or-later；板卡、显示器、平台标识和第三方组件仍受各自许可约束。
