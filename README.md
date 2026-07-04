# epass_usb_responder

电子通行证 自定义USB协议实现程序。通过 **bulk 端点** 与主机通信：按设备根文件树做文件读写/列表等操作，并可执行命令（带超时与输出上限）。应用层为自定义二进制帧（魔数 `EPAS`、CRC32），详见仓库根目录 [PROTOCOL.md](PROTOCOL.md)。

主要目的是实现usb连接手机等功能。做了文件传输和命令执行。

本仓库是100%的Vibe Coding代码，由GPT5.5和Claude Opus 4.7 联合编写。

## 仓库内容

| 路径 | 说明 |
|------|------|
| `src/` | 设备端守护进程源码（CMake 目标名：`epass_usb_responder`） |
| `scripts/usb_responder_gadget.sh` | 用 configfs 拉起 gadget、挂载 FunctionFS 并启动守护进程 |
| `pyhost/` | Python 上位机（`pyusb`），安装后提供命令 `epass-host` |

## 构建（设备端）

需要 CMake、C11 工具链；链接 `librt`（POSIX 消息队列等）。

```bash
cmake -S . -B build
cmake --build build
```

可执行文件默认在 `build/usb_responder`。若使用 `scripts/usb_responder_gadget.sh`，请用环境变量 **`USB_RESPONDER`** 指向实际二进制路径（脚本默认名可能与你的安装不一致）。

## 运行要点

1. 内核需启用 USB gadget、FunctionFS 等相关选项；由 root 或具备权限的用户操作 configfs。
2. 按内核约定：**先**挂载 FunctionFS 并启动本程序，**再**绑定 UDC；停止时顺序相反。脚本 `usb_responder_gadget.sh start|stop` 已按此编排。
3. 程序用法：

```text
epass_usb_responder --ffs <FunctionFS 挂载目录>
  [-v|--verbose] [--timeout-ms N] [--max-stdout N] [--max-stderr N]
```

`-v` / `--verbose` 打开 stderr 调试日志（帧头、ep0 事件、收发等待等）；默认静默，仅保留 `perror` 等关键错误与程序退出码。

文件协议路径从设备 `/` 解析；例如 `assets/a.bin` 对应 `/assets/a.bin`，`sd/assets/a.bin` 对应 `/sd/assets/a.bin`。`devinfo` 会报告 `/sd` 是否挂载到 mmc 设备以及 NAND/SD 容量。

## 上位机（PC）

在 `pyhost/` 目录下用你喜欢的 Python 环境安装依赖后，使用 `epass-host` 与设备通信（全局参数 `--vid/--pid/--serial/--interface/--timeout`，各子命令参数见 `--help`）。

| 子命令 | 说明 |
|--------|------|
| `hello` | 握手，打印 STATUS KV |
| `devinfo` | 读取设备信息（model/kernel/rootfs/app/存储状态） |
| `put <local> <remote>` | 上传单个文件；`--desire-storage nand\|sd`、`--perm <octal>`、`--chunk N` |
| `cp <src...> <dest>` | 批量上传，类 `cp` 语义：`dest` 为目录（尾斜杠或设备上确为目录）时复制为 `dest/源文件名`；`-r` 递归上传目录；默认保留本地文件原权限（`--perm` 覆盖、`--no-perm` 关闭，均不涉及 owner） |
| `get <remote> <local>` | 下载文件 |
| `ls [path]` | 列目录 |
| `stat <path>` | 路径元数据（owner/perm/size/type） |
| `rm <path>` | 删除文件或目录（目录递归）；`--desire-storage` |
| `mv <src> <dst>` | 重命名；`--desire-storage` |
| `mkdir <path>` | 创建目录；`-p/--parents`、`--desire-storage` |
| `exec -- <cmd...>` | 在设备上执行 shell 命令（带超时与输出上限） |

可在 shell 配置里加 `alias epcp='epass-host cp'`，用 `epcp aaa.txt /root/` 这样快速上传。

## 协议

完整字段与消息类型见 [PROTOCOL.md](PROTOCOL.md)。
