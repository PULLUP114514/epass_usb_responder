# usb_responder 协议说明（v1）

本文档描述 `usb_responder` 当前实现的 USB 应用层协议，供 PC/Android 客户端对接使用。

## 1. 总体设计

- 传输层：FunctionFS，单接口，2 个 bulk 端点（IN/OUT）。
- 应用层：自定义二进制帧（小端序，length-prefix，CRC32）。
- 协议版本：`1`（帧头 `version` 字段）。
- 魔数：`0x45504153`（ASCII: `EPAS`）。

## 2. 帧结构

每一帧由固定 24 字节头 + payload 组成。

```text
offset  size  field
0       4     magic (u32, little-endian) = 0x45504153
4       2     version (u16) = 1
6       2     type (u16)
8       4     flags (u32)  // 当前实现预留，通常为 0
12      4     request_id (u32)
16      4     payload_len (u32)
20      4     payload_crc32 (u32, 对 payload 计算)
24      N     payload
```

校验规则：
- `magic/version` 不匹配 => 丢弃。
- `payload_len` 与实际长度不一致 => 丢弃。
- `payload_crc32` 不匹配 => 丢弃。

## 3. 消息类型

### 3.1 基础类型

- `1` `HELLO`
- `2` `STATUS`
- `3` `ERROR`

### 3.2 文件相关

- `10` `FILE_PUT_BEGIN`
- `11` `FILE_PUT_CHUNK`
- `12` `FILE_PUT_END`
- `13` `FILE_GET`
- `14` `FILE_LIST`
- `15` `FILE_DELETE`
- `16` `FILE_RENAME`

### 3.3 命令执行

- `20` `COMMAND_EXEC`
- `21` `COMMAND_RESULT`

## 4. KV payload 编码（用于大部分控制类消息）

很多消息 payload 使用 KV 编码：

```text
u16 count
repeat count times:
  u16 key_len
  u16 value_len
  bytes[key_len]   key (UTF-8, 无 '\0')
  bytes[value_len] value (UTF-8, 无 '\0')
```

常见键：
- `path`
- `from`
- `to`
- `status`
- `message`
- `entries`

## 5. 典型交互流程

## 5.1 握手

1) Host -> `HELLO`  
2) Device -> `STATUS`（`service=usb_responder`, `version=1`）

## 5.2 上传文件

1) `FILE_PUT_BEGIN`，KV: `path=<relative_path>`  
2) 多次 `FILE_PUT_CHUNK`，payload 为文件分片  
   - 若分片前 4 字节是 transfer_id（小端），设备会优先使用它；否则使用帧头 `request_id`。  
3) `FILE_PUT_END`（可选 4 字节 transfer_id）  
4) 每步成功返回 `STATUS`，失败返回 `ERROR`

设备端行为：
- 上传先写入 `*.part` 临时文件；
- `FILE_PUT_END` 成功后 `rename` 到最终文件，避免半文件污染。

## 5.3 下载文件

1) Host -> `FILE_GET`，KV: `path=<relative_path>`  
2) Device -> `FILE_GET`，payload 为文件原始字节  
   - 失败返回 `ERROR`

## 5.4 列目录

1) Host -> `FILE_LIST`，KV: `path=.`（或子目录）  
2) Device -> `STATUS`，KV: `entries=<按行分隔文件名>`

## 5.5 删除/重命名

- 删除：`FILE_DELETE`，KV: `path=<relative_path>`
- 重命名：`FILE_RENAME`，KV: `from=<path1>`, `to=<path2>`

成功返回 `STATUS`，失败返回 `ERROR`。

## 6. 命令执行帧

`COMMAND_EXEC` payload（二进制）：

```text
u32 timeout_ms
u32 max_stdout
u32 max_stderr
u32 command_len
bytes[command_len] command_utf8
```

设备执行方式：`/bin/sh -c <command>`（开发板场景默认开放）

`COMMAND_RESULT` payload（二进制）：

```text
u32 exit_code
u8  timed_out   // 0 或 1
u8  reserved[3]
u32 duration_ms
u32 stdout_len
u32 stderr_len
bytes[stdout_len] stdout
bytes[stderr_len] stderr
```

超时处理：
- 先向进程组发送 `SIGTERM`；
- 若仍未退出，升级 `SIGKILL`；
- `timed_out=1`，并返回已捕获的输出与退出码。

## 7. 路径与安全约束

- 仅允许相对路径，不允许以 `/` 开头。
- 禁止 `..` 路径穿越。
- 文件操作根目录由启动参数 `--media-root` 指定。

## 8. 错误语义

- 业务错误统一返回 `ERROR`，KV: `message=<错误文本>`。
- 协议层错误（魔数、长度、CRC 不合法）通常直接丢帧或中断当前会话。

## 9. 端点与 FunctionFS 说明

- 程序参数 `--ffs` 指向 FunctionFS 挂载目录（例如 `/dev/ffs-epass`）。
- 程序会打开：
  - `ep0`：写 descriptors/strings，收事件
  - `ep1`：bulk IN（设备 -> 主机）
  - `ep2`：bulk OUT（主机 -> 设备）
- 在收到 `FUNCTIONFS_ENABLE` 后进入数据收发。
- 当前 descriptors 使用 vendor-specific 单接口、2 个 bulk 端点。全速端点包大小为 64，高速为 512。

### 9.1 USB request 分片与帧边界

协议帧边界不等于 USB bulk transfer 边界。一个协议帧可能被拆成多个 USB request，也可能多个小帧在内核/用户态缓冲中连续出现。因此两端实现必须按帧头 `payload_len` 重新组帧，不应假设一次 `read()` 就得到一个完整协议帧。

当前工程实现采用以下策略：

- `usb_responder` 设备端从 `ep2` 读取时，单次 `read()` 限制为 `16 KiB`，读到的数据进入内部 RX buffer，再按 24 字节头和 `payload_len` 切出完整帧。
- `usb_responder` 设备端向 `ep1` 写响应时，也按 `16 KiB` 分片写出。
- `pyhost` 主机端写 OUT 时按 `16 KiB` 分片；上传文件默认单个 `FILE_PUT_CHUNK` 的文件数据约为 `16 KiB - 4`（前 4 字节用于 `transfer_id`）。
- `pyhost` 主机端读 IN 时维护内部 RX buffer，按协议帧长度重组。

这些分片大小不是协议的一部分，只是当前实现为了兼容 F1C/full-speed/低内存 Linux gadget 的工程选择。其他客户端可以使用不同大小，但建议单个 USB request 不要过大，尤其不要一次向 FunctionFS `read()` 或 `write()` 申请数 MB 缓冲。

### 9.2 FunctionFS 低内存注意事项

在 F1C 等小内存设备上，FunctionFS 内核路径会按用户态 `read()`/`write()` 请求长度分配缓冲。如果设备端一次 `read(ep2, 8MB)`，可能触发内核 `__alloc_pages` warning 并返回 `ENOMEM`。因此设备端实现应使用较小固定块读取，再在用户态组帧。

常见现象与原因：

- `functionfs read size N > requested size 24`：用户态只请求 24 字节帧头，但主机实际发来了更大的 USB request。解决方式是设备端使用 RX buffer 按较大块读取并自行切帧。
- `ep_out read: Cannot allocate memory`：用户态单次 FunctionFS `read()` 请求过大，内核无法分配对应缓冲。
- `Cannot send after transport endpoint shutdown`：USB 断开、重新配置或 gadget disable 时端点被关闭，属于正常生命周期事件。当前实现会静默关闭端点并等待下一次 `FUNCTIONFS_ENABLE`。

## 10. Windows WinUSB 说明

当前实现在 FunctionFS descriptors 中携带 Microsoft OS 描述符：
- Extended Compat ID: `WINUSB`
- Extended Property: `DeviceInterfaceGUIDs`

注意：Windows 自动绑定 WinUSB 还依赖 gadget/configfs 侧启用 OS descriptor（`os_desc/use`、`b_vendor_code`、`qw_sign` 及配置链接）。该部分由系统接入侧完成。

## 11. Gadget 启动脚本（参考）

仓库内提供示例脚本，完成 configfs 挂载、gadget 创建、`os_desc`、FunctionFS 挂载并后台启动 `usb_responder`：

- `scripts/usb_responder_gadget.sh start`
- `scripts/usb_responder_gadget.sh stop`

可通过环境变量覆盖 `FFS_INSTANCE`、`FFS_MOUNT`、`MEDIA_ROOT`、`USB_RESPONDER`、`ID_VENDOR`、`ID_PRODUCT` 等，详见脚本头部注释。

## 12. pyhost 上位机（当前工程实现）

仓库内 `pyhost/` 是基于 `pyusb` 的参考上位机，使用 `uv` 管理虚拟环境：

```bash
cd pyhost
uv sync
uv run python -m pyhost --vid 0x1d6b --pid 0x0203 hello
uv run python -m pyhost --vid 0x1d6b --pid 0x0203 ls .
uv run python -m pyhost --vid 0x1d6b --pid 0x0203 put ./local.bin remote.bin
uv run python -m pyhost --vid 0x1d6b --pid 0x0203 get remote.bin ./out.bin
uv run python -m pyhost --vid 0x1d6b --pid 0x0203 exec -- uname -a
```

也可以使用 `pyproject.toml` 中注册的入口：

```bash
uv run epass-host --vid 0x1d6b --pid 0x0203 hello
```

`--vid/--pid` 由 gadget/configfs 侧决定，示例脚本默认是 `0x1d6b:0x0203`。如同时存在多个设备，可用 `--serial` 进一步筛选。

## 13. 工程化与排错建议

- 部署新版本后需要替换对侧实际运行的 `USB_RESPONDER` 路径（脚本默认 `/usr/bin/usb_responder`），并重启 gadget。
- 默认 `MEDIA_ROOT=/mnt`。上传返回 `begin upload failed: fopen ...` 时，优先检查该目录是否存在、是否可写、是否为只读文件系统。
- 文件路径只允许相对路径；绝对路径和 `..` 会被拒绝。
- 主机端若看到 `ERROR`，应解析 KV 中的 `message` 并展示给用户。当前设备端会尽量返回底层 errno 文本，便于定位权限、目录不存在、空间不足等问题。
- 不建议在全速 USB 上使用过大的上传 chunk。当前 `pyhost` 默认约 16KiB，吞吐和稳定性在 F1C 这类设备上更均衡。
- 原有 `build/` 目录若 CMake cache 记录了旧路径，可以新建独立构建目录，例如：

```bash
cmake -S . -B build-cursor
cmake --build build-cursor
ctest --test-dir build-cursor --output-on-failure
```
