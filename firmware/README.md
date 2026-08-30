# Firmware

AZORIA Touch 的 ESP32-S3 + LVGL 固件，提供亮度、音量、静音和输入源控制。

## 构建

```bash
pio run
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200
```

普通用户使用已预装固件的设备，在 AZORIA Desktop 的“AZORIA Touch”页完成
2.4 GHz Wi‑Fi 配网。开发者需要刷写或恢复固件时，先在桌面端设置中开启
“开发者模式”。BLE OTA 会校验固件大小与 SHA-256 后再重启。

连入 Wi‑Fi 后，Touch 只在 UDP 8733 被动等待经过签名的 Desktop 发现请求；身份
校验成功后才接受同一私网来源下发的 Desktop 地址和控制端口。Touch 不主动扫描
局域网主机，也不会连接公网地址。

当前硬件验证：VIEWE UEDX48480040E-WB-A V1.3、480×480 GC9503、FT6336U、16MB
Flash 和 8MB PSRAM。

代码遵循根目录 GPL-3.0-or-later；板卡、显示器、平台标识和第三方组件仍受各自许可约束。
