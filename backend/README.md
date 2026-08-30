# Backend

Python 本地后台，负责显示器控制、设备发现、USB 配网和 BLE/Wi‑Fi 请求转发。

## 启动

```bash
cp config.example.json config.local.json
python3 -m azoria_displayd --config config.local.json
```

后台默认监听 `8732`，控制接口需要配置中的 Bearer Token。`/setup`、Wi‑Fi 扫描和
Wi‑Fi 写入仅允许本机访问。

## 控制接口

```json
POST /v1/control
{"control":"brightness","value":42,"final":true}
```

支持的控制项：`brightness`、`volume`、`mute`、`input`。

状态读取：`GET /v1/status`；设备登记：`POST /v1/device/register`。

## 许可证

本目录遵循根目录 GPL-3.0-or-later。第三方依赖和 LG、Apple、Windows 等名称及标识
不由本项目许可证转让。
