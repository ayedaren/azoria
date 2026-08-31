# 显示器控制功能

- `service.*`：被动响应 Desktop 发现、同步显示器状态，并通过 UDP 协调广播或 BLE
  提交带命令 ID 的控制操作。
- `screen.*`：AZORIA Touch 的显示器控制界面与触控交互。

对外符号位于 `DisplayControl` 命名空间，其他模块不应引用本目录内部状态。

拖动过程发送的预览操作是尽力而为；松手后的最终操作等待 Desktop 确认，并在超时期间
以相同命令 ID 重发。Desktop 负责 Master 选择和重复命令缓存，Touch 不承担仲裁职责。
