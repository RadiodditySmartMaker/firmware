#!/usr/bin/env python3
"""
Upload `chinese_font.bin` directly to the device external QSPI flash over the
Meshtastic serial protobuf API using the existing device-side XMODEM transport.

The device firmware must include the matching XMODEM/QSPI receiver support.

Usage:
  python3 bin/upload_chinese_font_to_device.py --port /dev/tty.usbmodemXXXX
  python3 bin/upload_chinese_font_to_device.py --port /dev/tty.usbmodemXXXX --baud 115200
  python3 bin/upload_chinese_font_to_device.py --port /dev/tty.usbmodemXXXX --input bin/chinese_font.bin
"""

from __future__ import annotations

import argparse
import os
import time
from pathlib import Path

try:
    import serial
    from serial import SerialException
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover
    raise SystemExit("缺少 pyserial，请先执行: python3 -m pip install pyserial") from exc


START1 = 0x94
START2 = 0xC3

CTRL_SOH = 1
CTRL_EOT = 4
CTRL_ACK = 6
CTRL_NAK = 21
CTRL_CAN = 24

TORADIO_XMODEM_TAG = 5
TORADIO_WANT_CONFIG_ID_TAG = 3
TORADIO_HEARTBEAT_TAG = 7
FROMRADIO_CONFIG_COMPLETE_ID_TAG = 7
FROMRADIO_XMODEM_TAG = 12
FROMRADIO_QUEUESTATUS_TAG = 11

XMODEM_CONTROL_TAG = 1
XMODEM_SEQ_TAG = 2
XMODEM_CRC16_TAG = 3
XMODEM_BUFFER_TAG = 4
HEARTBEAT_NONCE_TAG = 1

PACKET_SIZE = 128
TARGET_NAME = b"qspi://chinese_font.bin"
MAX_FRAME_SIZE = 512

CHFONT_MAGIC = 0x43484631
CHFONT_VERSION = 1
CHFONT_MAX_BYTES = 0x00080000
CHFONT_KEY_SIZE = 4
CHFONT_BITMAP_SIZE = 32
SPECIAL_NONCE_ONLY_CONFIG = 69420


def crc16_ccitt(data: bytes) -> int:
    crc = 0
    for value in data:
        crc = ((crc >> 8) | ((crc & 0xFF) << 8)) & 0xFFFF
        crc ^= value
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc & 0xFFFF


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def decode_varint(buf: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(buf):
        byte = buf[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7
    raise ValueError("truncated varint")


def skip_field(buf: bytes, offset: int, wire_type: int) -> int:
    if wire_type == 0:
        _, offset = decode_varint(buf, offset)
        return offset
    if wire_type == 2:
        length, offset = decode_varint(buf, offset)
        return offset + length
    raise ValueError(f"unsupported wire type: {wire_type}")


def encode_varint_field(tag: int, value: int) -> bytes:
    return encode_varint((tag << 3) | 0) + encode_varint(value)


def encode_bytes_field(tag: int, payload: bytes) -> bytes:
    return encode_varint((tag << 3) | 2) + encode_varint(len(payload)) + payload


def encode_xmodem_packet(control: int, seq: int, payload: bytes) -> bytes:
    packet = bytearray()
    packet += encode_varint_field(XMODEM_CONTROL_TAG, control)
    if seq:
        packet += encode_varint_field(XMODEM_SEQ_TAG, seq)
    if payload:
        packet += encode_varint_field(XMODEM_CRC16_TAG, crc16_ccitt(payload))
        packet += encode_bytes_field(XMODEM_BUFFER_TAG, payload)
    return bytes(packet)


def encode_toradio_xmodem(control: int, seq: int = 0, payload: bytes = b"") -> bytes:
    body = encode_bytes_field(TORADIO_XMODEM_TAG, encode_xmodem_packet(control, seq, payload))
    return bytes((START1, START2, (len(body) >> 8) & 0xFF, len(body) & 0xFF)) + body


def encode_toradio_heartbeat(nonce: int) -> bytes:
    heartbeat = encode_varint_field(HEARTBEAT_NONCE_TAG, nonce)
    body = encode_bytes_field(TORADIO_HEARTBEAT_TAG, heartbeat)
    return bytes((START1, START2, (len(body) >> 8) & 0xFF, len(body) & 0xFF)) + body


def encode_toradio_want_config(nonce: int) -> bytes:
    body = encode_varint_field(TORADIO_WANT_CONFIG_ID_TAG, nonce)
    return bytes((START1, START2, (len(body) >> 8) & 0xFF, len(body) & 0xFF)) + body


def parse_int_auto(value: str) -> int:
    return int(value, 0)


def parse_xmodem_message(buf: bytes) -> dict[str, int | bytes]:
    result: dict[str, int | bytes] = {"control": 0, "seq": 0, "crc16": 0, "buffer": b""}
    offset = 0
    while offset < len(buf):
        key, offset = decode_varint(buf, offset)
        tag = key >> 3
        wire_type = key & 0x07
        if tag == XMODEM_CONTROL_TAG and wire_type == 0:
            result["control"], offset = decode_varint(buf, offset)
        elif tag == XMODEM_SEQ_TAG and wire_type == 0:
            result["seq"], offset = decode_varint(buf, offset)
        elif tag == XMODEM_CRC16_TAG and wire_type == 0:
            result["crc16"], offset = decode_varint(buf, offset)
        elif tag == XMODEM_BUFFER_TAG and wire_type == 2:
            length, offset = decode_varint(buf, offset)
            result["buffer"] = buf[offset : offset + length]
            offset += length
        else:
            offset = skip_field(buf, offset, wire_type)
    return result


def extract_fromradio_xmodem(buf: bytes) -> dict[str, int | bytes] | None:
    offset = 0
    while offset < len(buf):
        key, offset = decode_varint(buf, offset)
        tag = key >> 3
        wire_type = key & 0x07
        if tag == FROMRADIO_XMODEM_TAG and wire_type == 2:
            length, offset = decode_varint(buf, offset)
            return parse_xmodem_message(buf[offset : offset + length])
        offset = skip_field(buf, offset, wire_type)
    return None


def has_fromradio_tag(buf: bytes, tag_wanted: int) -> bool:
    offset = 0
    while offset < len(buf):
        key, offset = decode_varint(buf, offset)
        tag = key >> 3
        wire_type = key & 0x07
        if tag == tag_wanted:
            return True
        offset = skip_field(buf, offset, wire_type)
    return False


def extract_fromradio_varint(buf: bytes, tag_wanted: int) -> int | None:
    offset = 0
    while offset < len(buf):
        key, offset = decode_varint(buf, offset)
        tag = key >> 3
        wire_type = key & 0x07
        if tag == tag_wanted and wire_type == 0:
            value, offset = decode_varint(buf, offset)
            return value
        offset = skip_field(buf, offset, wire_type)
    return None


def read_frame(port: serial.Serial, timeout_s: float) -> bytes | None:
    deadline = time.monotonic() + timeout_s
    state = 0
    while time.monotonic() < deadline:
        raw = port.read(1)
        if not raw:
            continue
        byte = raw[0]
        if state == 0:
            state = 1 if byte == START1 else 0
        elif state == 1:
            state = 2 if byte == START2 else (1 if byte == START1 else 0)
        else:
            length_hi = byte
            length_lo = port.read(1)
            if not length_lo:
                state = 0
                continue
            payload_len = (length_hi << 8) | length_lo[0]
            if payload_len > MAX_FRAME_SIZE:
                state = 0
                continue
            payload = port.read(payload_len)
            if len(payload) != payload_len:
                state = 0
                continue
            return payload
    return None


def wait_for_xmodem_control(port: serial.Serial, timeout_s: float) -> dict[str, int | bytes]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        payload = read_frame(port, max(0.1, deadline - time.monotonic()))
        if not payload:
            continue
        xmodem = extract_fromradio_xmodem(payload)
        if xmodem is not None:
            return xmodem
    raise TimeoutError("等待设备响应超时")


def wait_for_api_frame(port: serial.Serial, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        payload = read_frame(port, max(0.1, deadline - time.monotonic()))
        if not payload:
            continue
        if has_fromradio_tag(payload, FROMRADIO_QUEUESTATUS_TAG) or payload:
            return
    raise TimeoutError("等待串口 API 响应超时")


def ensure_api_ready(port: serial.Serial, retries: int, timeout_s: float) -> None:
    print("等待设备串口 API 就绪...")
    for attempt in range(1, retries + 1):
        heartbeat = encode_toradio_heartbeat(attempt)
        port.write(heartbeat)
        port.flush()
        try:
            wait_for_api_frame(port, timeout_s)
            return
        except TimeoutError:
            if attempt >= retries:
                raise
            time.sleep(0.5)


def wait_for_config_complete(port: serial.Serial, timeout_s: float, nonce: int) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        payload = read_frame(port, max(0.1, deadline - time.monotonic()))
        if not payload:
            continue
        config_complete_id = extract_fromradio_varint(payload, FROMRADIO_CONFIG_COMPLETE_ID_TAG)
        if config_complete_id is None:
            continue
        if config_complete_id != nonce:
            raise RuntimeError(f"收到意外的 config_complete_id: {config_complete_id}, 期望 {nonce}")
        return
    raise TimeoutError("等待设备完成 API 配置握手超时")


def ensure_packet_mode(port: serial.Serial, retries: int, timeout_s: float) -> None:
    print("初始化 Meshtastic API 会话...")
    request = encode_toradio_want_config(SPECIAL_NONCE_ONLY_CONFIG)
    for attempt in range(1, retries + 1):
        port.write(request)
        port.flush()
        try:
            wait_for_config_complete(port, timeout_s=timeout_s, nonce=SPECIAL_NONCE_ONLY_CONFIG)
            print("设备已进入数据包传输状态。")
            return
        except TimeoutError:
            if attempt >= retries:
                raise
            time.sleep(0.5)


def validate_font_image(blob: bytes, magic_expected: int, version_expected: int, key_size: int, bitmap_size: int,
                        max_bytes: int) -> None:
    if len(blob) < 16:
        raise ValueError("字体文件过小，缺少头部")
    if len(blob) > max_bytes:
        raise ValueError(f"字体文件过大: {len(blob)} > {max_bytes}")

    magic = int.from_bytes(blob[0:4], "little")
    version = int.from_bytes(blob[4:8], "little")
    count = int.from_bytes(blob[8:12], "little")
    if magic != magic_expected:
        raise ValueError(f"magic 不匹配: 0x{magic:08x}")
    if version != version_expected:
        raise ValueError(f"version 不匹配: {version}")
    if count == 0:
        raise ValueError("count 为 0")

    expected = 16 + count * key_size + count * bitmap_size
    if expected != len(blob):
        raise ValueError(f"文件大小不匹配: 头部推导 {expected}，实际 {len(blob)}")


def send_with_retry(
    port: serial.Serial,
    frame: bytes,
    expected_seq: int,
    retries: int,
    timeout_s: float,
) -> None:
    for attempt in range(1, retries + 1):
        port.write(frame)
        port.flush()
        try:
            response = wait_for_xmodem_control(port, timeout_s)
        except TimeoutError:
            if attempt < retries:
                continue
            raise
        control = int(response["control"])
        seq = int(response["seq"])
        if control == CTRL_ACK:
            if seq not in (0, expected_seq):
                raise RuntimeError(f"收到 ACK，但序号异常: {seq}, 期望 {expected_seq}")
            return
        if control == CTRL_NAK:
            if attempt < retries:
                continue
            raise RuntimeError(f"设备连续返回 NAK，序号 {expected_seq}")
        if control == CTRL_CAN:
            raise RuntimeError("设备取消了传输")
        raise RuntimeError(f"收到未知控制码: {control}")
    raise RuntimeError("发送失败")


def upload_font(port: serial.Serial, blob: bytes, retries: int, erase_timeout: float, packet_timeout: float,
                target_name: bytes) -> None:
    port.reset_input_buffer()
    port.reset_output_buffer()

    print("开始握手并请求设备进入中文字库上传模式...")
    start_frame = encode_toradio_xmodem(CTRL_SOH, 0, target_name)
    send_with_retry(port, start_frame, expected_seq=0, retries=retries, timeout_s=erase_timeout)

    total_packets = (len(blob) + PACKET_SIZE - 1) // PACKET_SIZE
    for index, offset in enumerate(range(0, len(blob), PACKET_SIZE), start=1):
        chunk = blob[offset : offset + PACKET_SIZE]
        frame = encode_toradio_xmodem(CTRL_SOH, index, chunk)
        send_with_retry(port, frame, expected_seq=index, retries=retries, timeout_s=packet_timeout)
        print(f"\r已发送 {index}/{total_packets} 包 ({offset + len(chunk)}/{len(blob)} 字节)", end="", flush=True)

    print()
    print("发送结束标记...")
    eot_frame = encode_toradio_xmodem(CTRL_EOT, 0, b"")
    send_with_retry(port, eot_frame, expected_seq=0, retries=retries, timeout_s=packet_timeout)
    print("上传完成，设备已确认写入。")


def wait_for_port(port_name: str, wait_seconds: float) -> None:
    deadline = time.monotonic() + wait_seconds
    while time.monotonic() < deadline:
        if os.path.exists(port_name):
            return
        time.sleep(0.5)
    raise TimeoutError(f"等待串口 {port_name} 出现超时")


def get_port_candidates(port_name: str) -> list[str]:
    candidates = [port_name]
    if port_name.startswith("/dev/cu."):
        candidates.insert(0, "/dev/tty." + port_name[len("/dev/cu.") :])
    elif port_name.startswith("/dev/tty."):
        candidates.append("/dev/cu." + port_name[len("/dev/tty.") :])

    unique: list[str] = []
    for candidate in candidates:
        if candidate not in unique:
            unique.append(candidate)
    return unique


def collect_runtime_port_candidates(port_name: str) -> list[str]:
    base_candidates = get_port_candidates(port_name)
    base_names = {Path(candidate).name for candidate in base_candidates}
    port_infos = list(list_ports.comports())

    for info in port_infos:
        device = info.device
        name = Path(device).name

        if device in base_candidates:
            continue

        same_usbmodem_family = name.startswith("tty.usbmodem") or name.startswith("cu.usbmodem")
        shares_base = any(base_name.startswith("tty.usbmodem") or base_name.startswith("cu.usbmodem") for base_name in base_names)

        if shares_base and same_usbmodem_family:
            base_candidates.append(device)

    unique: list[str] = []
    for candidate in base_candidates:
        if candidate not in unique:
            unique.append(candidate)
    return unique


def open_serial_with_retry(port_name: str, baud: int, timeout: float, write_timeout: float, wait_seconds: float) -> serial.Serial:
    deadline = time.monotonic() + wait_seconds
    last_error: Exception | None = None
    candidates = collect_runtime_port_candidates(port_name)

    while time.monotonic() < deadline:
        for candidate in candidates:
            if not os.path.exists(candidate):
                continue
            try:
                port = serial.Serial(candidate, baud, timeout=timeout, write_timeout=write_timeout)
                return port
            except (OSError, SerialException) as exc:
                last_error = exc
        time.sleep(0.5)

    detail = f": {last_error}" if last_error else ""
    raise TimeoutError(f"等待串口可打开超时{detail}")


def connect_api_port(port_name: str, baud: int, wait_seconds: float, boot_wait: float, api_retries: int,
                     api_timeout: float, config_timeout: float) -> serial.Serial:
    deadline = time.monotonic() + wait_seconds
    last_error: Exception | None = None

    while time.monotonic() < deadline:
        candidates = collect_runtime_port_candidates(port_name)
        for candidate in candidates:
            if not os.path.exists(candidate):
                continue
            try:
                port = serial.Serial(candidate, baud, timeout=0.2, write_timeout=5)
                time.sleep(boot_wait)
                try:
                    ensure_api_ready(port, retries=api_retries, timeout_s=api_timeout)
                    ensure_packet_mode(port, retries=max(2, min(api_retries, 4)), timeout_s=config_timeout)
                    print(f"已连接设备串口 API: {candidate}")
                    return port
                except Exception:
                    port.close()
                    last_error = TimeoutError(f"{candidate} 没有返回串口 API 响应")
            except (OSError, SerialException) as exc:
                last_error = exc
        time.sleep(0.8)

    detail = f": {last_error}" if last_error else ""
    raise TimeoutError(f"等待可用的设备串口 API 超时{detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload chinese_font.bin to external QSPI flash over Meshtastic serial API")
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/tty.usbmodemXXXX")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--input", default="bin/chinese_font.bin", help="Path to chinese_font.bin")
    parser.add_argument("--target-name", default=TARGET_NAME.decode("utf-8"), help="Device-side XMODEM target name")
    parser.add_argument("--magic", type=parse_int_auto, default=CHFONT_MAGIC, help="Expected image magic")
    parser.add_argument("--version", type=int, default=CHFONT_VERSION, help="Expected image version")
    parser.add_argument("--key-size", type=int, default=CHFONT_KEY_SIZE, help="UTF-8 key size in bytes")
    parser.add_argument("--bitmap-size", type=int, default=CHFONT_BITMAP_SIZE, help="Bitmap size per glyph in bytes")
    parser.add_argument("--max-bytes", type=parse_int_auto, default=CHFONT_MAX_BYTES, help="Maximum accepted image size")
    parser.add_argument("--retries", type=int, default=8, help="Retry count for each packet")
    parser.add_argument("--erase-timeout", type=float, default=20.0, help="Timeout for the initial erase/prepare step")
    parser.add_argument("--packet-timeout", type=float, default=5.0, help="Timeout for each data packet")
    parser.add_argument("--wait-seconds", type=float, default=20.0, help="Wait time for the serial port to reappear")
    parser.add_argument("--boot-wait", type=float, default=3.0, help="Extra settle time after opening the serial port")
    parser.add_argument("--config-timeout", type=float, default=15.0, help="Timeout for the initial API config handshake")
    args = parser.parse_args()

    input_path = Path(args.input)
    blob = input_path.read_bytes()
    validate_font_image(
        blob,
        magic_expected=args.magic,
        version_expected=args.version,
        key_size=args.key_size,
        bitmap_size=args.bitmap_size,
        max_bytes=args.max_bytes,
    )
    target_name = args.target_name.encode("utf-8")

    print(f"输入文件: {input_path}")
    print(f"文件大小: {len(blob)} 字节")
    print(f"串口: {args.port} @ {args.baud}")
    print(f"目标: {args.target_name}")

    wait_for_port(args.port, args.wait_seconds)

    with connect_api_port(
        args.port,
        args.baud,
        wait_seconds=args.wait_seconds,
        boot_wait=args.boot_wait,
        api_retries=max(3, min(args.retries, 8)),
        api_timeout=5.0,
        config_timeout=args.config_timeout,
    ) as port:
        upload_font(
            port,
            blob,
            retries=args.retries,
            erase_timeout=args.erase_timeout,
            packet_timeout=args.packet_timeout,
            target_name=target_name,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
