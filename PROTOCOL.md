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
- `17` `FILE_MKDIR`
- `18` `FILE_STAT`

### 3.3 命令执行

- `20` `COMMAND_EXEC`
- `21` `COMMAND_RESULT`

### 3.4 设备信息

- `30` `DEVINFO`

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
- `parents`（可选：`1` / `true` / `yes` 表示创建父目录，见 `FILE_MKDIR`）
- `desire_storage`（可选：`nand` / `sd`，写入类 API 的期望存储校验）
- `status`
- `message`
- `files` / `dirs`（`FILE_LIST` 成功应答：按行分隔的条目名）
- `owner` / `perm` / `size` / `type`（`FILE_STAT` 应答，见下文）

## 5. 典型交互流程

## 5.1 握手

1) Host -> `HELLO`  
2) Device -> `STATUS`（`service=usb_responder`, `version=1`）

## 5.2 上传文件

1) `FILE_PUT_BEGIN`，KV: `path=<relative_path>`，可选 `desire_storage=nand|sd`  
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
2) Device -> `STATUS`，**仅**两个键：  
   - `files`：非目录项（普通文件、符号链接、设备等）名称，每行一个，UTF-8  
   - `dirs`：子目录名称，每行一个，UTF-8  
   不含 `.` / `..`；某一类为空时对应值为空字符串。

## 5.4.1 路径状态（stat）

1) Host -> `FILE_STAT`，KV: `path=<relative_path>`  
2) Device -> `STATUS`，KV：  
   - `owner`：`用户名:组名`；若无 passwd/group 解析则退化为 `uid:gid`（十进制）  
   - `perm`：低位权限，4 位八进制（`st_mode & 07777`，如 `0644`、`0755`）  
   - `size`：十进制 `st_size`（目录为目录项所占块统计的内核语义，`lstat` 结果）  
   - `type`：`file` / `dir` / `link` / `other`

失败返回 `ERROR`。

## 5.5 删除 / 创建目录 / 重命名

- 删除路径：`FILE_DELETE`，KV: `path=<relative_path>`，可选 `desire_storage=nand|sd`  
  - 普通文件或符号链接：unlink。  
  - 目录：**递归删除**其下所有内容，再 `rmdir`（等价于受限根目录内的 `rm -rf`）。
- 创建目录：`FILE_MKDIR`，KV: `path=<relative_path>`，可选 `parents=1`（或 `true`/`yes`）、`desire_storage=nand|sd`  
  - 无 `parents`：只建最后一级（父目录须已存在）。  
  - 有 `parents`：沿路径逐级 `mkdir`（类似 `mkdir -p`）。
- 重命名：`FILE_RENAME`，KV: `from=<path1>`, `to=<path2>`，可选 `desire_storage=nand|sd`
  - 若 `from` 与 `to` 位于不同存储，返回 `ERROR`；客户端应改用复制后删除。

成功返回 `STATUS`，失败返回 `ERROR`。

## 5.6 设备信息

1) Host -> `DEVINFO`，无 payload
2) Device -> `DEVINFO`，KV：

   - `model`：来自 `/proc/device-tree/model`（已裁剪 NUL 与末尾空白）
   - `kernel`：来自 `uname(2)` 的 `release` 字段
   - `rootfs`：`/etc/os-release` 全文（多行，UTF-8）
   - `app`：`/root/epass_drm_app version` 的标准输出（trim）
   - `sd_mounted`：`1` 表示 `/sd` 当前挂载到 mmc 设备，`0` 表示未挂载或来源不是 mmc
   - `nand_total_bytes` / `nand_free_bytes`：`/` 所在文件系统容量和可用空间，十进制字节数
   - `sd_total_bytes` / `sd_free_bytes`：`/sd` 已挂载到 mmc 时的容量和可用空间；未挂载时为 `0`

任一字段获取失败返回空字符串；不会因为某项失败而整体失败。
`app` 字段执行有 3000ms 超时，超时或非零退出按空处理。

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
- 文件操作按设备根文件树解析：`foo/bar` 映射到 `/foo/bar`，`sd/foo` 映射到 `/sd/foo`。
- `sd` 或 `sd/...` 被视为 SD 存储路径；其他路径被视为 NAND 路径。
- SD 写入类操作要求 `/sd` 当前确认为 mmc 挂载，否则返回 `ERROR`，避免 SD 未挂载时误写入 NAND 上的 `/sd` 目录。
- 写入类 API 的 `desire_storage` 只做校验，不参与路径重写；期望存储和路径实际存储不一致时返回 `ERROR`。

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

可通过环境变量覆盖 `FFS_INSTANCE`、`FFS_MOUNT`、`USB_RESPONDER`、`ID_VENDOR`、`ID_PRODUCT` 等，详见脚本头部注释。

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
- 文件协议路径默认从设备 `/` 解析。上传返回 `begin upload failed: fopen ...` 时，优先检查目标目录是否存在、是否可写、文件系统是否只读；写入 `sd/...` 时还要检查 `/sd` 是否挂载到 mmc。
- 文件路径只允许相对路径；绝对路径和 `..` 会被拒绝。
- 主机端若看到 `ERROR`，应解析 KV 中的 `message` 并展示给用户。当前设备端会尽量返回底层 errno 文本，便于定位权限、目录不存在、空间不足等问题。
- 不建议在全速 USB 上使用过大的上传 chunk。当前 `pyhost` 默认约 16KiB，吞吐和稳定性在 F1C 这类设备上更均衡。
- 原有 `build/` 目录若 CMake cache 记录了旧路径，可以新建独立构建目录，例如：

```bash
cmake -S . -B build-cursor
cmake --build build-cursor
ctest --test-dir build-cursor --output-on-failure
```
