# KVM 后端模块

- `api.py`：KVM 状态、控制、设备登记和本地配网接口。
- `config.py`：KVM 后端、显示器和接口安全配置。
- `controller.py`：DDC/CI 控制、读回确认、缓存和 macOS 电源兜底。
- `lg_usb.py`：LG 显示器独立 USB HID 通道封装。
- `setup.html`：通过 USB 为触摸屏配置 Wi-Fi 的本机页面。
- `discovery.py`：仅在显式私有局域网模式下提供 mDNS 和 UDP 自动发现。

模块只依赖 `azoria_displayd.core` 中的 USB 配网基础设施。
