#!/bin/sh
#
# usb_responder_gadget.sh — 启动/停止 FunctionFS + configfs USB gadget，供 usb_responder 使用。
#
# 用法:
#   usb_responder_gadget.sh start [选项]
#   usb_responder_gadget.sh stop
#
# 环境变量（均有默认值，可按板子覆盖）:
#   CONFIGFS_DIR      默认 /sys/kernel/config
#   GADGET_NAME       默认 g1
#   FFS_INSTANCE      FunctionFS 实例名，须与 mount 第三参数一致，默认 epass
#   FFS_MOUNT         挂载点，默认 /dev/ffs-epass
#   MEDIA_ROOT        usb_responder --media-root，默认 /mnt
#   USB_RESPONDER     可执行文件路径，默认 epass_usb_responder
#   USB_RESPONDER_VERBOSE  设为 1/true/yes 时启动加 -v（详细日志）
#   ID_VENDOR         默认 0x1d6b
#   ID_PRODUCT        默认 0x0203（与常见 Linux Foundation 测试 PID 不同，请按需改）
#   MS_VENDOR_CODE    OS 描述符 b_vendor_code，默认 0xcd（须与主机侧 WCID 探测一致）
#
# 注意:
#   1) WinUSB 自动绑定除 FunctionFS 内 MS OS 描述符外，还需本脚本打开 gadget 级 os_desc。
#   2) 若与其它 gadget（MTP/ACM）互斥，请先 stop 本脚本再切模式。
#   3) 须在绑定 UDC 之前 mount functionfs 并启动 usb_responder（与内核约定一致）。

set -eu

CONFIGFS_DIR="${CONFIGFS_DIR:-/sys/kernel/config}"
GADGET_NAME="${GADGET_NAME:-g1}"
GADGET_ROOT="${CONFIGFS_DIR}/usb_gadget/${GADGET_NAME}"
FFS_INSTANCE="${FFS_INSTANCE:-epass}"
FFS_MOUNT="${FFS_MOUNT:-/dev/ffs-epass}"
MEDIA_ROOT="${MEDIA_ROOT:-/mnt}"
USB_RESPONDER="${USB_RESPONDER:-epass_usb_responder}"
ID_VENDOR="${ID_VENDOR:-0x1d6b}"
ID_PRODUCT="${ID_PRODUCT:-0x0203}"
MS_VENDOR_CODE="${MS_VENDOR_CODE:-0xcd}"

UDC_AUTO="${UDC_AUTO:-1}"

die() {
    echo "error: $*" >&2
    exit 1
}

first_udc() {
    # shellcheck disable=SC2012
    ls /sys/class/udc/ 2>/dev/null | head -n 1 || true
}

RESPONDER_PIDFILE="${RESPONDER_PIDFILE:-/var/run/usb_responder.pid}"

kill_responder() {
    if command -v start-stop-daemon >/dev/null 2>&1; then
        start-stop-daemon -K -q -p "$RESPONDER_PIDFILE" -x "$USB_RESPONDER" 2>/dev/null || true
    elif [ -f "$RESPONDER_PIDFILE" ]; then
        kill "$(cat "$RESPONDER_PIDFILE")" 2>/dev/null || true
        rm -f "$RESPONDER_PIDFILE"
    fi
}

gadget_stop() {
    kill_responder
    if [ -d "$GADGET_ROOT" ]; then
        if [ -f "$GADGET_ROOT/UDC" ]; then
            echo "" > "$GADGET_ROOT/UDC" 2>/dev/null || true
            sleep 1
        fi
    fi
    umount "$FFS_MOUNT" 2>/dev/null || true
    if [ -d "$GADGET_ROOT" ]; then
        # 只 unlink 我们建的 symlink；不要动 os_desc 下的内核属性文件
        rm -f "$GADGET_ROOT"/os_desc/c.1 2>/dev/null || true
        rm -f "$GADGET_ROOT"/configs/c.1/ffs."$FFS_INSTANCE" 2>/dev/null || true
        rmdir "$GADGET_ROOT"/functions/ffs."$FFS_INSTANCE" 2>/dev/null || true
        rmdir "$GADGET_ROOT"/configs/c.1/strings/0x409 2>/dev/null || true
        rmdir "$GADGET_ROOT"/configs/c.1 2>/dev/null || true
        rmdir "$GADGET_ROOT"/strings/0x409 2>/dev/null || true
        rmdir "$GADGET_ROOT" 2>/dev/null || true
    fi
    rmdir "$FFS_MOUNT" 2>/dev/null || true
}

gadget_start() {
    if [ ! -d "$CONFIGFS_DIR" ]; then
        mount -t configfs none "$CONFIGFS_DIR" || die "无法挂载 configfs 到 $CONFIGFS_DIR"
    fi

    gadget_stop

    mkdir -p "$GADGET_ROOT"
    cd "$GADGET_ROOT" || die "无法进入 $GADGET_ROOT"

    echo "$ID_VENDOR" > idVendor
    echo "$ID_PRODUCT" > idProduct
    echo 0x0100 > bcdUSB
    echo 0x0200 > bcdDevice

    mkdir -p strings/0x409
    echo "Rhodes Island" > strings/0x409/manufacturer
    echo "ePass USB Responder" > strings/0x409/product
    echo "00000000" > strings/0x409/serialnumber

    mkdir -p configs/c.1/strings/0x409
    echo "Config 1" > configs/c.1/strings/0x409/configuration
    echo 120 > configs/c.1/MaxPower

    # 创建 FunctionFS 实例并挂到配置（注意：configfs 解析 symlink 目标相对当前 cwd）
    mkdir -p "functions/ffs.$FFS_INSTANCE"
    if [ ! -e "configs/c.1/ffs.$FFS_INSTANCE" ]; then
        ln -s "functions/ffs.$FFS_INSTANCE" "configs/c.1/"
    fi

    # Microsoft OS 1.0 描述符（配合 usb_responder 内 FUNCTIONFS_HAS_MS_OS_DESC）
    # os_desc 由内核在创建 gadget 时自动生成，不要 mkdir。
    if [ -d os_desc ]; then
        echo 1 > os_desc/use
        echo "$MS_VENDOR_CODE" > os_desc/b_vendor_code
        # qw_sign 须为 8 字节： "MSFT100" + NUL
        printf 'MSFT100\0' > os_desc/qw_sign
        # configfs 把 symlink 目标按当前 cwd 解析，故用 "configs/c.1"
        if [ ! -e os_desc/c.1 ]; then
            ln -s configs/c.1 os_desc/ 2>/dev/null \
                || echo "warning: 无法创建 os_desc -> c.1 链接，Windows 可能不会自动绑定 WinUSB" >&2
        fi
    else
        echo "warning: $GADGET_ROOT/os_desc 不存在，跳过 WinUSB 描述符" >&2
    fi

    mkdir -p "$FFS_MOUNT"
    mount -t functionfs "$FFS_INSTANCE" "$FFS_MOUNT" || die "无法 mount functionfs $FFS_INSTANCE -> $FFS_MOUNT"

    RESPONDER_VERBOSE_ARG=
    case "${USB_RESPONDER_VERBOSE:-}" in
        1|true|yes) RESPONDER_VERBOSE_ARG="-v" ;;
    esac

    # if command -v start-stop-daemon >/dev/null 2>&1; then
    #     start-stop-daemon -S -q -m -b -p "$RESPONDER_PIDFILE" -x "$USB_RESPONDER" -- \
    #         --ffs "$FFS_MOUNT" \
    #         --media-root "$MEDIA_ROOT" \
    #         "$@"
    # else
        "$USB_RESPONDER" \
            $RESPONDER_VERBOSE_ARG \
            --ffs "$FFS_MOUNT" \
            --media-root "$MEDIA_ROOT" \
            "$@" &
        echo $! > "$RESPONDER_PIDFILE"
    # fi

    sleep 1

    if [ "$UDC_AUTO" = "1" ]; then
        u="$(first_udc)"
        if [ -z "$u" ]; then
            die "未找到 UDC（/sys/class/udc/ 为空）"
        fi
        echo "$u" > UDC || die "绑定 UDC 失败: $u"
    else
        echo "UDC_AUTO=0，请手动: echo <udc_name> > $GADGET_ROOT/UDC"
    fi

    echo "OK: gadget=$GADGET_ROOT ffs=$FFS_MOUNT responder=$USB_RESPONDER"
}

cmd="${1:-}"
[ "$#" -gt 0 ] && shift

case "$cmd" in
    start)
        gadget_start "$@"
        ;;
    stop)
        gadget_stop
        echo "OK: stopped"
        ;;
    *)
        die "用法: $0 {start|stop} [start 时传给 usb_responder 的额外参数]"
        ;;
esac
