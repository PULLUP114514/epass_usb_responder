from __future__ import annotations

import argparse
import struct
import sys
from typing import Dict, List, Optional, Tuple

import usb.core
import usb.util

import protocol as P

MAX_PAYLOAD = 8 * 1024 * 1024
USB_REQUEST_CHUNK = 16 * 1024
DEFAULT_CHUNK = USB_REQUEST_CHUNK - 4


def _write_exact(ep, data: bytes, timeout: int) -> None:
    sent = 0
    while sent < len(data):
        n = ep.write(data[sent : sent + USB_REQUEST_CHUNK], timeout=timeout)
        if n <= 0:
            raise IOError("USB write failed")
        sent += n


def struct_pack_u32(v: int) -> bytes:
    return v.to_bytes(4, "little")


def struct_unpack_header(hdr: bytes) -> Tuple[int, int, int, int, int, int, int]:
    return struct.unpack("<IHHIIII", hdr)


class UsbResponderClient:
    def __init__(
        self,
        vid: int,
        pid: int,
        *,
        serial: Optional[str] = None,
        interface: int = 0,
        timeout_ms: int = 60_000,
    ) -> None:
        self._dev = usb.core.find(
            idVendor=vid,
            idProduct=pid,
            custom_match=lambda d: serial is None or d.serial_number == serial,
        )
        if self._dev is None:
            raise RuntimeError(
                f"未找到设备 vid=0x{vid:04x} pid=0x{pid:04x}" + (f" serial={serial!r}" if serial else "")
            )
        self._timeout = timeout_ms
        self._iface = interface
        try:
            if self._dev.is_kernel_driver_active(interface):
                self._dev.detach_kernel_driver(interface)
        except (NotImplementedError, ValueError, usb.core.USBError):
            pass
        self._dev.set_configuration()
        cfg = self._dev.get_active_configuration()
        intf = usb.util.find_descriptor(cfg, bInterfaceNumber=interface)
        if intf is None:
            raise RuntimeError(f"无接口 {interface}")
        self._ep_in = usb.util.find_descriptor(
            intf,
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN,
        )
        self._ep_out = usb.util.find_descriptor(
            intf,
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT,
        )
        if self._ep_in is None or self._ep_out is None:
            raise RuntimeError("未找到 bulk IN/OUT 端点（期望 0x81 / 0x02）")
        self._req = 0
        self._rxbuf = bytearray()
        self._rx_read_size = USB_REQUEST_CHUNK

    def close(self) -> None:
        usb.util.dispose_resources(self._dev)

    def _next_id(self) -> int:
        self._req = (self._req + 1) & 0xFFFFFFFF
        if self._req == 0:
            self._req = 1
        return self._req

    def _send_frame(self, typ: int, payload: bytes = b"", req_id: Optional[int] = None) -> int:
        rid = self._next_id() if req_id is None else req_id
        raw = P.Frame(type=typ, request_id=rid, payload=payload).encode()
        _write_exact(self._ep_out, raw[: P.HEADER_SIZE], self._timeout)
        if len(raw) > P.HEADER_SIZE:
            _write_exact(self._ep_out, raw[P.HEADER_SIZE :], self._timeout)
        return rid

    def _recv_frame(self) -> P.Frame:
        while len(self._rxbuf) < P.HEADER_SIZE:
            self._read_some(self._rx_read_size)
        hdr = bytes(self._rxbuf[: P.HEADER_SIZE])
        _magic, _ver, _typ, _fl, _rid, plen, _crc = struct_unpack_header(hdr)
        if plen > MAX_PAYLOAD:
            raise ValueError("payload too large")
        frame_size = P.HEADER_SIZE + plen
        while len(self._rxbuf) < frame_size:
            self._read_some(frame_size - len(self._rxbuf))
        raw = bytes(self._rxbuf[:frame_size])
        del self._rxbuf[:frame_size]
        return P.decode_frame(raw)

    def _read_some(self, size: int) -> None:
        chunk = self._ep_in.read(size, timeout=self._timeout)
        if chunk is None or len(chunk) == 0:
            raise IOError("USB read timeout or empty")
        self._rxbuf.extend(bytes(chunk))

    def _expect_kv(self, req_id: int) -> Dict[str, str]:
        fr = self._recv_frame()
        if fr.request_id != req_id:
            raise RuntimeError(f"request_id 不匹配: 期望 {req_id} 收到 {fr.request_id}")
        if fr.type == P.MSG_ERROR:
            kv = P.decode_kv(fr.payload)
            raise RuntimeError(kv.get("message", "ERROR"))
        if fr.type != P.MSG_STATUS:
            raise RuntimeError(f"意外消息类型 {fr.type}，期望 STATUS")
        return P.decode_kv(fr.payload)

    def hello(self) -> Dict[str, str]:
        rid = self._send_frame(P.MSG_HELLO)
        return self._expect_kv(rid)

    def file_put(self, local_path: str, remote_path: str, chunk_size: int = DEFAULT_CHUNK) -> None:
        rid = self._next_id()
        begin = P.encode_kv([("path", remote_path)])
        self._send_frame(P.MSG_FILE_PUT_BEGIN, begin, req_id=rid)
        self._expect_kv(rid)

        with open(local_path, "rb") as f:
            while True:
                piece = f.read(chunk_size)
                if not piece:
                    break
                chunk_payload = struct_pack_u32(rid) + piece
                self._send_frame(P.MSG_FILE_PUT_CHUNK, chunk_payload, req_id=rid)
                self._expect_kv(rid)

        self._send_frame(P.MSG_FILE_PUT_END, struct_pack_u32(rid), req_id=rid)
        self._expect_kv(rid)

    def file_get(self, remote_path: str, local_path: str) -> None:
        rid = self._send_frame(P.MSG_FILE_GET, P.encode_kv([("path", remote_path)]))
        fr = self._recv_frame()
        if fr.request_id != rid:
            raise RuntimeError(f"request_id 不匹配: 期望 {rid} 收到 {fr.request_id}")
        if fr.type == P.MSG_ERROR:
            kv = P.decode_kv(fr.payload)
            raise RuntimeError(kv.get("message", "ERROR"))
        if fr.type != P.MSG_FILE_GET:
            raise RuntimeError(f"意外类型 {fr.type}，期望 FILE_GET")
        with open(local_path, "wb") as out:
            out.write(fr.payload)

    def file_list(self, path: str = ".") -> List[str]:
        rid = self._send_frame(P.MSG_FILE_LIST, P.encode_kv([("path", path)]))
        kv = self._expect_kv(rid)
        ent = kv.get("entries", "")
        if not ent.strip():
            return []
        return ent.splitlines()

    def file_delete(self, remote_path: str) -> None:
        rid = self._send_frame(P.MSG_FILE_DELETE, P.encode_kv([("path", remote_path)]))
        self._expect_kv(rid)

    def file_rename(self, src: str, dst: str) -> None:
        rid = self._send_frame(P.MSG_FILE_RENAME, P.encode_kv([("from", src), ("to", dst)]))
        self._expect_kv(rid)

    def command_exec(
        self,
        command: str,
        *,
        timeout_ms: int = 0,
        max_stdout: int = 0,
        max_stderr: int = 0,
    ) -> P.CommandResult:
        pl = P.encode_command_exec(
            command, timeout_ms=timeout_ms, max_stdout=max_stdout, max_stderr=max_stderr
        )
        rid = self._send_frame(P.MSG_COMMAND_EXEC, pl)
        fr = self._recv_frame()
        if fr.request_id != rid:
            raise RuntimeError(f"request_id 不匹配: 期望 {rid} 收到 {fr.request_id}")
        if fr.type == P.MSG_ERROR:
            kv = P.decode_kv(fr.payload)
            raise RuntimeError(kv.get("message", "ERROR"))
        if fr.type != P.MSG_COMMAND_RESULT:
            raise RuntimeError(f"意外类型 {fr.type}，期望 COMMAND_RESULT")
        return P.decode_command_result(fr.payload)


def _parse_hex(s: str) -> int:
    s = s.strip().lower()
    if s.startswith("0x"):
        s = s[2:]
    return int(s, 16)


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="usb_responder 上位机（pyusb），协议见 PROTOCOL.md")
    p.add_argument("--vid", type=_parse_hex, default=0x1d6b, help="USB Vendor ID（十六进制，如 0x1234）")
    p.add_argument("--pid", type=_parse_hex, default=0x0203, help="USB Product ID")
    p.add_argument("--serial", default=None, help="按序列号筛选（可选）")
    p.add_argument("--interface", type=int, default=0, help="接口号，默认 0")
    p.add_argument("--timeout", type=int, default=60_000, help="USB 传输超时（毫秒）")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("hello", help="握手，打印 STATUS KV")

    sp = sub.add_parser("put", help="上传文件")
    sp.add_argument("local")
    sp.add_argument("remote")
    sp.add_argument("--chunk", type=int, default=DEFAULT_CHUNK, help="每片字节数（默认约 16KiB）")

    sg = sub.add_parser("get", help="下载文件")
    sg.add_argument("remote")
    sg.add_argument("local")

    sl = sub.add_parser("ls", help="列目录")
    sl.add_argument("path", nargs="?", default=".")

    sd = sub.add_parser("rm", help="删除文件")
    sd.add_argument("path")

    sm = sub.add_parser("mv", help="重命名")
    sm.add_argument("src")
    sm.add_argument("dst")

    se = sub.add_parser("exec", help="在设备上执行 shell 命令（/bin/sh -c）")
    se.add_argument("--shell-timeout", type=int, default=0, help="设备侧超时 ms（0=设备默认）")
    se.add_argument("--max-stdout", type=int, default=0, help="stdout 上限（0=设备默认）")
    se.add_argument("--max-stderr", type=int, default=0, help="stderr 上限（0=设备默认）")
    se.add_argument("args", nargs=argparse.REMAINDER, help="命令（若含 - 开头请加 --）")

    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        cl = UsbResponderClient(
            args.vid, args.pid, serial=args.serial, interface=args.interface, timeout_ms=args.timeout
        )
    except Exception as e:
        print(e, file=sys.stderr)
        return 1
    try:
        if args.cmd == "hello":
            kv = cl.hello()
            for k in sorted(kv.keys()):
                print(f"{k}={kv[k]}")
        elif args.cmd == "put":
            cl.file_put(args.local, args.remote, chunk_size=args.chunk)
        elif args.cmd == "get":
            cl.file_get(args.remote, args.local)
        elif args.cmd == "ls":
            for name in cl.file_list(args.path):
                print(name)
        elif args.cmd == "rm":
            cl.file_delete(args.path)
        elif args.cmd == "mv":
            cl.file_rename(args.src, args.dst)
        elif args.cmd == "exec":
            cmd = " ".join(args.args).strip()
            if not cmd:
                print("exec: 需要命令", file=sys.stderr)
                return 2
            r = cl.command_exec(
                cmd,
                timeout_ms=args.shell_timeout,
                max_stdout=args.max_stdout,
                max_stderr=args.max_stderr,
            )
            sys.stdout.buffer.write(r.stdout)
            sys.stderr.buffer.write(r.stderr)
            if r.timed_out:
                print(f"\n[timed_out duration_ms={r.duration_ms}]", file=sys.stderr)
            ec = r.exit_code
            if ec < 0 or ec > 255:
                return 1
            return ec
        return 0
    except Exception as e:
        print(e, file=sys.stderr)
        return 1
    finally:
        cl.close()


if __name__ == "__main__":
    raise SystemExit(main())
