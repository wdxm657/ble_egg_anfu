"""
Radar prediction debug visualizer.

- Serial: text lines after 8-char log prefix, e.g. PREV,...,RAW,... / PRED,STA,... / PREDSEQ,...
- BLE: binary EVENT on Ctrl TX (cmd 0x57): prediction debug + BOUNDARY_PT (0x04).
  BOUNDARY_PT is **not** streamed: device sends 4 NOTIFY only after CMD **0x58** on Ctrl RX.
  **CMD 0x59** (RADAR_TRACK_SPEED): APP writes Ctrl RX with payload u16 LE ``interval_us`` (µs)
  to set radar track gimbal step interval (same as ``StepMotor_GimbalSetSpeedUs``).
  The night-mode UI can send 0x59 from the right panel.
  Script defaults to requesting the quad on connect; use --no-ble-request-boundary to skip.
  The UI updates the black quad only after **all four** corners are received.
  pip install bleak
  (Bleak 0.x uses get_services(); 1.0+ discovers on connect and exposes client.services.)

Why BLE might drop soon after connect (not caused by this script "giving up"):

1. Firmware requests conn params including supervision timeout ~4s (app.c
   bls_l2cap_requestConnParamUpdate(..., CONN_TIMEOUT_4S)). If the Windows
   radio/host misses connection events for longer than that, the link times out
   (HCI connection timeout).

2. If start_notify fails (wrong UUID, permissions), Bleak exits the client
   context and the connection ends; older code hid this with a bare except.

This script reconnects in a loop and prints errors to stderr.
"""

import argparse
import asyncio
import math
import queue
import sys
import threading
import time
import traceback
import uuid
from collections import deque
from typing import List, Optional, Set, Tuple

import serial
from PyQt5 import QtCore, QtGui, QtWidgets
from PyQt5.QtCore import Qt

import matplotlib

matplotlib.use("Qt5Agg")
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
from matplotlib.patches import FancyArrowPatch
from visualizer import commands as vc
from visualizer import protocol as cp

# Default serial (when --transport serial)
PORT = "COM3"
BAUDRATE = 115200

# Ctrl TX notify (same octet order as app_att.c CUSTOM_CTRL_TX_CHAR_UUID).
CTRL_TX_RAW_BYTES = bytes(
    [
        0x02,
        0xA0,
        0x0D,
        0x0C,
        0x0B,
        0x0A,
        0x09,
        0x08,
        0x07,
        0x06,
        0x05,
        0x04,
        0x03,
        0x02,
        0x01,
        0x00,
    ]
)
# RFC-4122 string for those bytes (used as first guess; Windows may expose reversed 128-bit order).
CTRL_TX_UUID = str(uuid.UUID(bytes=CTRL_TX_RAW_BYTES))

# Ctrl RX write (01 A0 ... per app_att.c)
CTRL_RX_RAW_BYTES = bytes(
    [
        0x01,
        0xA0,
        0x0D,
        0x0C,
        0x0B,
        0x0A,
        0x09,
        0x08,
        0x07,
        0x06,
        0x05,
        0x04,
        0x03,
        0x02,
        0x01,
        0x00,
    ]
)
CTRL_RX_UUID = str(uuid.UUID(bytes=CTRL_RX_RAW_BYTES))


def _ctrl_tx_uuid_int_candidates() -> Set[int]:
    """Same logical characteristic under different 128-bit byte orders."""
    return {
        uuid.UUID(bytes=CTRL_TX_RAW_BYTES).int,
        uuid.UUID(bytes=CTRL_TX_RAW_BYTES[::-1]).int,
    }


def _characteristic_uuid_int(char) -> Optional[int]:
    try:
        u = char.uuid
        if isinstance(u, uuid.UUID):
            return u.int
        return uuid.UUID(str(u)).int
    except Exception:
        return None


def _ctrl_rx_uuid_int_candidates() -> Set[int]:
    return {
        uuid.UUID(bytes=CTRL_RX_RAW_BYTES).int,
        uuid.UUID(bytes=CTRL_RX_RAW_BYTES[::-1]).int,
    }


def _find_ctrl_tx_characteristic(client) -> Optional[object]:
    """Return Bleak GATT characteristic for Ctrl TX, or None."""
    targets = _ctrl_tx_uuid_int_candidates()
    for svc in client.services:
        for char in svc.characteristics:
            ci = _characteristic_uuid_int(char)
            if ci is not None and ci in targets:
                return char
    return None


def _find_ctrl_rx_characteristic(client) -> Optional[object]:
    """Return Bleak GATT characteristic for Ctrl RX (write), or None."""
    targets = _ctrl_rx_uuid_int_candidates()
    for svc in client.services:
        for char in svc.characteristics:
            ci = _characteristic_uuid_int(char)
            if ci is not None and ci in targets:
                return char
    return None


def _dump_gatt_table(client) -> None:
    print("[BLE] GATT dump (service -> characteristics):", file=sys.stderr)
    try:
        for svc in client.services:
            su = str(svc.uuid)
            for char in svc.characteristics:
                props = ",".join(char.properties) if char.properties else ""
                print(f"  {su} -> {char.uuid} [{props}]", file=sys.stderr)
    except Exception as ex:
        print(f"  (dump failed: {ex})", file=sys.stderr)


async def _bleak_ensure_gatt_ready(client) -> None:
    """Bleak 0.x: ``await get_services()``. Bleak 1.0+: GATT on connect, use ``client.services``."""
    gs = getattr(client, "get_services", None)
    if gs is not None and callable(gs):
        out = gs()
        if asyncio.iscoroutine(out):
            await out
        return
    # Touch collection so we fail clearly if discovery did not run (per bleak docs).
    _ = client.services


# Optional keepalive: standard GAP Device Name (read-only on most peripherals)
GAP_DEVICE_NAME_UUID = "00002a00-0000-1000-8000-00805f9b34fb"

CTRL_PROTO_VERSION = cp.CTRL_PROTO_VERSION
CTRL_MSG_TYPE_CMD = cp.CTRL_MSG_TYPE_CMD
CTRL_MSG_TYPE_EVENT = cp.CTRL_MSG_TYPE_EVENT
CTRL_MSG_TYPE_RSP = cp.CTRL_MSG_TYPE_RSP
CTRL_CMD_TEXT_CHUNK = cp.CTRL_CMD_TEXT_CHUNK
CTRL_CMD_RADAR_DEBUG_GET_BOUNDARY = cp.CTRL_CMD_RADAR_DEBUG_GET_BOUNDARY
CTRL_CMD_RADAR_TRACK_SPEED = cp.CTRL_CMD_RADAR_TRACK_SPEED

# x: [-1100, 1100], y: [100, 4100]
X_MIN, X_MAX = -2500, 2500
Y_MIN, Y_MAX = 100, 7000

# From RAW point, arrow length in mm (matches firmware sin/cos(motion_rad) step convention)
MOTION_ARROW_MM = 480.0

BOUNDARY_QUAD = [
    (-1000, 800),
    (1000, 800),
    (1000, 4000),
    (-1000, 4000),
]

# 与固件 StepMotor_ClampIntervalUs 下限一致；上位机发送前也会夹紧
TRACK_INTERVAL_US_MIN = 750
TRACK_INTERVAL_US_MAX = 20000
TRACK_INTERVAL_DEFAULT_US = 800

RADAR_BOUNDARY_ERR_TEXT = {
    0: "OK",
    1: "EDGE_TOO_SHORT(<1m)",
    2: "ORDER_INVALID",
    3: "STATE_INVALID",
    4: "INDEX_INVALID",
}


def build_ctrl_cmd_frame(cmd_id: int, seq: int, payload: bytes) -> bytes:
    return cp.build_ctrl_cmd_frame(cmd_id, seq, payload)


class RadarVisualizer:
    """Shared state; background thread reads serial or BLE notifications."""

    def __init__(
        self,
        transport: str,
        port: str = PORT,
        baudrate: int = BAUDRATE,
        ble_address: Optional[str] = None,
        ble_list_gatt: bool = False,
        ble_request_boundary: bool = True,
    ):
        self._transport = transport
        self._stop = threading.Event()
        self._lock = threading.Lock()

        self._ser = None
        self._ble_address = ble_address
        self._ble_enabled = bool(ble_address)
        self._ble_list_gatt = ble_list_gatt
        self._ble_request_boundary = ble_request_boundary
        self._ble_thread: Optional[threading.Thread] = None
        self._ble_ctrl_tx_char: Optional[object] = None
        self._ble_ctrl_rx_char: Optional[object] = None
        self._ble_tx_queue = queue.Queue()
        self._ble_tx_seq: int = 0

        self.seq_history = deque(maxlen=9)

        self.latest_prev = None
        self.latest_raw = None
        self.latest_pred_a = None
        self.latest_pred_b = None

        self.motion_dir_valid = 0
        self.motion_dir_deg10 = 0

        self.boundary_quad: List[Tuple[int, int]] = [tuple(p) for p in BOUNDARY_QUAD]
        self.boundary_epoch = 0
        self._boundary_corner_rcv: List[Optional[Tuple[int, int]]] = [
            None,
            None,
            None,
            None,
        ]

        self.log_lines = deque(maxlen=2000)
        self.ctrl_lines = deque(maxlen=2000)

        # BLE text chunk reassembly (EVENT 0x40). Firmware streams in-order chunks.
        self._text_rx_transfer_id: Optional[int] = None
        self._text_rx_chunk_total: int = 0
        self._text_rx_next_chunk: int = 0
        self._text_rx_buf = bytearray()

        if transport == "serial":
            self._ser = serial.Serial(port=port, baudrate=baudrate, timeout=0.1)

    def send_radar_track_interval_us(self, interval_us: int) -> bool:
        """下发 CTRL_CMD_RADAR_TRACK_SPEED (0x59)，payload u16 LE interval_us。仅 BLE。"""
        if self._transport != "ble":
            return False
        v = max(TRACK_INTERVAL_US_MIN, min(TRACK_INTERVAL_US_MAX, int(interval_us)))
        self._ble_tx_seq = (self._ble_tx_seq + 1) & 0xFF
        pl = cp.u16le(v)
        frame = build_ctrl_cmd_frame(CTRL_CMD_RADAR_TRACK_SPEED, self._ble_tx_seq, pl)
        self._ble_tx_queue.put(frame)
        return True

    def send_cmd(self, cmd_id: int, payload: bytes, note: str = "") -> bool:
        if self._transport != "ble":
            return False
        self._ble_tx_seq = (self._ble_tx_seq + 1) & 0xFF
        frame = build_ctrl_cmd_frame(cmd_id, self._ble_tx_seq, payload)
        self._ble_tx_queue.put(frame)
        with self._lock:
            if note:
                self.ctrl_lines.append(
                    f"[TX] cmd=0x{cmd_id:02X} seq={self._ble_tx_seq} {note}"
                )
            else:
                self.ctrl_lines.append(
                    f"[TX] cmd=0x{cmd_id:02X} seq={self._ble_tx_seq} pl={payload.hex()}"
                )
        return True

    def start(self):
        if self._transport == "serial":
            t = threading.Thread(target=self._reader_loop_serial, daemon=True)
            t.start()
        else:
            self._ble_thread = threading.Thread(target=self._ble_worker, daemon=True)
            self._ble_thread.start()

    def set_ble_target(self, address: str, enabled: bool) -> None:
        with self._lock:
            self._ble_address = (address or "").strip()
            self._ble_enabled = bool(enabled)

    def ble_target(self) -> tuple[str, bool]:
        with self._lock:
            return (self._ble_address or ""), bool(self._ble_enabled)

    def close(self):
        self._stop.set()
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass

    def _reader_loop_serial(self):
        assert self._ser is not None
        while not self._stop.is_set():
            try:
                line = self._ser.readline().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue
                with self._lock:
                    self.log_lines.append(line)
                self._parse_line(line)
            except Exception:
                time.sleep(0.01)

    def _ble_worker(self):
        try:
            asyncio.run(self._ble_async())
        except Exception:
            print("[BLE] asyncio.run failed:", file=sys.stderr)
            traceback.print_exc()

    async def _ble_async(self):
        from bleak import BleakClient

        def on_notify(_handle, data: bytearray):
            b = bytes(data)
            self._apply_ble_frame(b)

        def on_disconnected(*_args):
            print("[BLE] stack reported disconnect", file=sys.stderr)

        # Reconnect loop: peripheral or host may drop link (supervision ~4s on this firmware).
        while not self._stop.is_set():
            address, enabled = self.ble_target()
            if (not enabled) or (not address):
                await asyncio.sleep(0.2)
                continue
            try:
                async with BleakClient(
                    address,
                    disconnected_callback=on_disconnected,
                ) as client:
                    print("[BLE] connected", file=sys.stderr)
                    await _bleak_ensure_gatt_ready(client)
                    if self._ble_list_gatt:
                        _dump_gatt_table(client)
                    ctrl_tx = _find_ctrl_tx_characteristic(client)
                    if ctrl_tx is None:
                        _dump_gatt_table(client)
                        raise RuntimeError(
                            "Ctrl TX characteristic not found (custom notify). "
                            "Confirm firmware is ble_cat_laser_toy and check GATT dump above."
                        )
                    self._ble_ctrl_tx_char = ctrl_tx
                    rx_ch = _find_ctrl_rx_characteristic(client)
                    self._ble_ctrl_rx_char = rx_ch
                    if rx_ch is None:
                        print(
                            "[BLE] Ctrl RX not found; track speed writes disabled",
                            file=sys.stderr,
                        )
                    print(f"[BLE] notify on {ctrl_tx.uuid}", file=sys.stderr)
                    await client.start_notify(ctrl_tx, on_notify)
                    if self._ble_request_boundary:

                        async def _write_get_boundary(rx_char) -> None:
                            req = bytes(
                                [
                                    CTRL_PROTO_VERSION,
                                    CTRL_MSG_TYPE_CMD,
                                    CTRL_CMD_RADAR_DEBUG_GET_BOUNDARY,
                                    0,
                                    0,
                                    0,
                                ]
                            )
                            try:
                                await client.write_gatt_char(
                                    rx_char, req, response=True
                                )
                            except Exception:
                                await client.write_gatt_char(
                                    rx_char, req, response=False
                                )

                        await asyncio.sleep(0.25)
                        try:
                            if rx_ch is None:
                                print(
                                    "[BLE] Ctrl RX not found; cannot request boundary quad",
                                    file=sys.stderr,
                                )
                            else:
                                await _write_get_boundary(rx_ch)
                                print("[BLE] sent GET_BOUNDARY (0x58)", file=sys.stderr)
                                await asyncio.sleep(1.0)
                                with self._lock:
                                    need_retry = self.boundary_epoch == 0
                                if need_retry:
                                    print(
                                        "[BLE] no BOUNDARY_PT yet; retry GET_BOUNDARY",
                                        file=sys.stderr,
                                    )
                                    await _write_get_boundary(rx_ch)
                        except Exception as ex:
                            print(
                                "[BLE] GET_BOUNDARY write failed:",
                                ex,
                                file=sys.stderr,
                            )
                    keepalive = 0
                    while not self._stop.is_set():
                        a2, e2 = self.ble_target()
                        if (not e2) or (not a2):
                            print("[BLE] disabled by UI, disconnecting", file=sys.stderr)
                            break
                        if not client.is_connected:
                            print(
                                "[BLE] is_connected=False, will reconnect",
                                file=sys.stderr,
                            )
                            break
                        try:
                            while True:
                                pkt = self._ble_tx_queue.get_nowait()
                                rxw = self._ble_ctrl_rx_char
                                if rxw is not None:
                                    try:
                                        await client.write_gatt_char(
                                            rxw, pkt, response=False
                                        )
                                    except Exception as ex:
                                        print(
                                            f"[BLE] write queue failed: {ex}",
                                            file=sys.stderr,
                                        )
                        except queue.Empty:
                            pass
                        keepalive += 1
                        # Light GATT read ~every 2s to nudge some Windows stacks / central scheduling.
                        if keepalive >= 40:
                            keepalive = 0
                            try:
                                await client.read_gatt_char(GAP_DEVICE_NAME_UUID)
                            except Exception:
                                pass
                        await asyncio.sleep(0.05)
                    try:
                        await client.stop_notify(ctrl_tx)
                    except Exception:
                        pass
                    self._ble_ctrl_tx_char = None
                    self._ble_ctrl_rx_char = None
            except Exception as ex:
                if self._stop.is_set():
                    break
                print("[BLE] session error (reconnecting):", ex, file=sys.stderr)
                traceback.print_exc()
                await asyncio.sleep(1.5)

    def _apply_ble_frame(self, data: bytes) -> None:
        # 打印16进制数据
        print(data.hex())
        fr = cp.parse_ctrl_frame(data)
        if fr is None:
            return
        if fr.version != CTRL_PROTO_VERSION:
            return
        cmd_id = fr.cmd_id
        payload = fr.payload

        if fr.msg_type == CTRL_MSG_TYPE_EVENT and cmd_id == CTRL_CMD_TEXT_CHUNK:
            if not payload:
                return
            # payload: [0]=transferId, [1]=chunkIndex, [2]=chunkTotal, [3]=dataLen, [4..]=data
            if len(payload) < 4:
                return
            transfer_id = int(payload[0])
            chunk_idx = int(payload[1])
            chunk_total = int(payload[2])
            data_len = int(payload[3])
            if data_len < 0 or (4 + data_len) > len(payload):
                return
            chunk_data = payload[4 : 4 + data_len]

            complete_lines = []
            parse_hex_line = None
            with self._lock:
                # Start (or restart) on chunk 0 or transfer id change.
                if (
                    chunk_idx == 0
                    or self._text_rx_transfer_id is None
                    or self._text_rx_transfer_id != transfer_id
                ):
                    self._text_rx_transfer_id = transfer_id
                    self._text_rx_chunk_total = max(1, chunk_total)
                    self._text_rx_next_chunk = 0
                    self._text_rx_buf = bytearray()

                # Enforce in-order assembly (firmware sends sequentially).
                if chunk_idx != self._text_rx_next_chunk:
                    self._text_rx_transfer_id = None
                    self._text_rx_chunk_total = 0
                    self._text_rx_next_chunk = 0
                    self._text_rx_buf = bytearray()
                    return

                self._text_rx_buf += chunk_data
                self._text_rx_next_chunk += 1

                if self._text_rx_next_chunk >= self._text_rx_chunk_total:
                    b = bytes(self._text_rx_buf)
                    self._text_rx_transfer_id = None
                    self._text_rx_chunk_total = 0
                    self._text_rx_next_chunk = 0
                    self._text_rx_buf = bytearray()
                    try:
                        s = b.decode("utf-8", errors="replace")
                        complete_lines = s.splitlines()
                        for line in complete_lines:
                            self.log_lines.append(line)
                    except Exception:
                        parse_hex_line = "[BLE][TEXT_CHUNK] " + b.hex()
                        self.log_lines.append(parse_hex_line)

            for line in complete_lines:
                self._parse_line(line)
            if parse_hex_line is not None:
                self._parse_line(parse_hex_line)
            return

        with self._lock:
            self.ctrl_lines.append(self._decode_ctrl_line(fr))

    def _decode_ctrl_line(self, fr: cp.CtrlFrame) -> str:
        pld = fr.payload
        if fr.msg_type == CTRL_MSG_TYPE_RSP:
            st = pld[0] if len(pld) >= 1 else -1
            if fr.cmd_id == cp.CTRL_CMD_POWER_CTRL and len(pld) >= 2:
                return f"[RSP][0x12] status={st} on={pld[1]}"
            if fr.cmd_id == cp.CTRL_CMD_MOTOR_DIR_CTRL and len(pld) >= 3:
                return f"[RSP][0x22] status={st} dir={pld[1]} op={pld[2]}"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_SET_INSTALL_HEIGHT:
                return f"[RSP][0x50] status={st}"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_BOUNDARY_ENTER:
                return f"[RSP][0x51] status={st}"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_BOUNDARY_SELECT_POINT and len(pld) >= 2:
                err = pld[1]
                return f"[RSP][0x52] status={st} idx/err={err}({RADAR_BOUNDARY_ERR_TEXT.get(err, 'UNKNOWN')})"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_BOUNDARY_SAVE_POINT and len(pld) >= 4:
                idx = pld[1]
                ready = pld[2]
                err = pld[3]
                return f"[RSP][0x53] status={st} idx={idx} ready={ready} err={err}({RADAR_BOUNDARY_ERR_TEXT.get(err, 'UNKNOWN')})"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_BOUNDARY_COMMIT and len(pld) >= 4:
                apply_ok = pld[1]
                err = pld[2]
                pair_mask = pld[3]
                return f"[RSP][0x55] status={st} apply={apply_ok} err={err}({RADAR_BOUNDARY_ERR_TEXT.get(err, 'UNKNOWN')}) pairMask=0x{pair_mask:02X}"
            if fr.cmd_id == cp.CTRL_CMD_RADAR_RESET_FLASH_CONFIG:
                return f"[RSP][0x56] status={st}"
            return f"[RSP][0x{fr.cmd_id:02X}] status={st} pl={pld.hex()}"

        if (
            fr.msg_type == CTRL_MSG_TYPE_EVENT
            and fr.cmd_id == cp.CTRL_CMD_MOTOR_DIR_CTRL
        ):
            if len(pld) >= 2 and pld[0] == 0x01:
                return f"[EVT][0x22] reached dir={pld[1]}"
            if len(pld) >= 4 and pld[0] == 0x02:
                return f"[EVT][0x22] boundary_guard dir={pld[1]} point={pld[2]} limit={pld[3]}"
            if len(pld) >= 4 and pld[0] == 0x03:
                tilt = pld[2] | (pld[3] << 8)
                if tilt >= 0x8000:
                    tilt -= 0x10000
                return f"[EVT][0x22] height_limit dir={pld[1]} tilt_deg10={tilt}"
        return f"[RX] type=0x{fr.msg_type:02X} cmd=0x{fr.cmd_id:02X} seq={fr.seq} pl={pld.hex()}"

    def _apply_boundary_pt(self, corner_idx: int, x_mm: int, y_mm: int) -> None:
        if corner_idx < 0 or corner_idx > 3:
            return
        with self._lock:
            self._boundary_corner_rcv[corner_idx] = (x_mm, y_mm)
            if all(self._boundary_corner_rcv[i] is not None for i in range(4)):
                self.boundary_quad = []
                for i in range(4):
                    pt = self._boundary_corner_rcv[i]
                    assert pt is not None
                    self.boundary_quad.append(pt)
                self.boundary_epoch += 1
                self._boundary_corner_rcv = [None, None, None, None]

    def _apply_prev_raw(
        self,
        prev_x: int,
        prev_y: int,
        raw_x: int,
        raw_y: int,
        motion_valid: int = 0,
        motion_deg10: int = 0,
    ) -> None:
        # High-rate stream: only refresh prev/raw. Do not clear STA / PREDSEQ (sticky for debug).
        with self._lock:
            self.latest_prev = (prev_x, prev_y)
            self.latest_raw = (raw_x, raw_y, None)
            self.motion_dir_valid = 1 if motion_valid else 0
            self.motion_dir_deg10 = int(motion_deg10)

    def _apply_pred_sta(self, ax: int, ay: int, bx: int, by: int) -> None:
        # Hold until next STA; do not clear seq trail (independent channel).
        with self._lock:
            self.latest_pred_a = (ax, ay)
            self.latest_pred_b = (bx, by)

    def _apply_predseq(self, idx: int, x: int, y: int) -> None:
        # Hold STA points while appending seq; new sequence only when idx==1.
        with self._lock:
            if idx == 1:
                self.seq_history.clear()
            self.seq_history.append((x, y))

    def _parse_line(self, line: str):
        s = line.strip()
        if not s:
            return
        if s.startswith("[") and "]" in s:
            s = s.split("]", 1)[1].strip()

        parts = [p.strip() for p in s.split(",")]
        if len(parts) < 2:
            return

        try:
            if parts[0] == "PREV" and len(parts) >= 6 and parts[3] == "RAW":
                m_valid = 0
                m_deg10 = 0
                if len(parts) >= 9 and parts[6] == "M":
                    m_valid = int(parts[7])
                    m_deg10 = int(parts[8])
                self._apply_prev_raw(
                    int(parts[1]),
                    int(parts[2]),
                    int(parts[4]),
                    int(parts[5]),
                    m_valid,
                    m_deg10,
                )
            elif parts[0] == "PRED" and parts[1] == "STA" and len(parts) >= 6:
                self._apply_pred_sta(
                    int(parts[2]), int(parts[3]), int(parts[4]), int(parts[5])
                )
            elif parts[0] == "PREDSEQ" and len(parts) >= 4:
                self._apply_predseq(int(parts[1]), int(parts[2]), int(parts[3]))
            elif parts[0] == "BND" and len(parts) >= 4:
                self._apply_boundary_pt(int(parts[1]), int(parts[2]), int(parts[3]))
        except ValueError:
            return


NIGHT_STYLESHEET = """
QMainWindow, QWidget { background-color: #1e1e2e; color: #cdd6f4; }
QPlainTextEdit {
  background-color: #181825; color: #cdd6f4; border: 1px solid #45475a;
  border-radius: 6px; padding: 8px; font-family: Consolas, "Courier New", monospace;
  font-size: 12px;
}
QGroupBox {
  font-weight: bold; border: 1px solid #45475a; border-radius: 8px; margin-top: 12px; padding: 12px;
}
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; color: #89b4fa; }
QSpinBox, QPushButton {
  background: #313244; color: #cdd6f4; border: 1px solid #45475a; border-radius: 4px; padding: 6px 12px;
}
QPushButton:hover { background: #45475a; }
QPushButton:disabled { color: #6c7086; background: #313244; }
QLabel { color: #bac2de; }
"""


class RadarNightWindow(QtWidgets.QMainWindow):
    """夜间模式：左侧坐标图，右侧雷达数据 + 跟踪步间隔下发 (0x59)。"""

    def __init__(self, vis: RadarVisualizer, title: str):
        super().__init__()
        self.vis = vis
        self._last_boundary_epoch = -1

        self.setWindowTitle(title)
        self.resize(1180, 820)
        self.setStyleSheet(NIGHT_STYLESHEET)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QHBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(12)

        splitter = QtWidgets.QSplitter(Qt.Horizontal)
        root.addWidget(splitter, 1)

        left_wrap = QtWidgets.QWidget()
        left_l = QtWidgets.QVBoxLayout(left_wrap)
        left_l.setContentsMargins(0, 0, 0, 0)
        self.figure = Figure(figsize=(6.5, 8), dpi=100, facecolor="#11111b")
        self.canvas = FigureCanvas(self.figure)
        left_l.addWidget(self.canvas, 1)
        splitter.addWidget(left_wrap)

        right = QtWidgets.QWidget()
        right_l = QtWidgets.QVBoxLayout(right)
        right_l.setContentsMargins(0, 0, 0, 0)
        right_l.setSpacing(10)

        lbl_data = QtWidgets.QLabel("雷达数据")
        lbl_data.setStyleSheet("font-size: 14px; color: #89b4fa; font-weight: bold;")
        right_l.addWidget(lbl_data)
        self.radar_text = QtWidgets.QPlainTextEdit()
        self.radar_text.setReadOnly(True)
        self.radar_text.setMinimumWidth(340)
        self.radar_text.document().setDefaultFont(
            QtGui.QFont("Consolas", 11)
            if sys.platform == "win32"
            else QtGui.QFont("monospace", 11)
        )
        right_l.addWidget(self.radar_text, 1)

        lbl_log = QtWidgets.QLabel("固件日志 (仅 0x40)")
        lbl_log.setStyleSheet("font-size: 14px; color: #89b4fa; font-weight: bold;")
        right_l.addWidget(lbl_log)
        self.log_text = QtWidgets.QPlainTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMinimumHeight(170)
        self.log_text.document().setDefaultFont(
            QtGui.QFont("Consolas", 10)
            if sys.platform == "win32"
            else QtGui.QFont("monospace", 10)
        )
        right_l.addWidget(self.log_text, 1)

        speed_box = QtWidgets.QGroupBox("跟踪速度 (BLE → 设备)")
        speed_l = QtWidgets.QVBoxLayout(speed_box)
        hint = QtWidgets.QLabel(
            "步进间隔 interval_us：数值越小电机越快（固件与 StepMotor_GimbalSetSpeedUs 一致）。"
            f" 允许 {TRACK_INTERVAL_US_MIN}～{TRACK_INTERVAL_US_MAX} µs。"
        )
        hint.setWordWrap(True)
        speed_l.addWidget(hint)
        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("interval_us:"))
        self.speed_spin = QtWidgets.QSpinBox()
        self.speed_spin.setRange(TRACK_INTERVAL_US_MIN, TRACK_INTERVAL_US_MAX)
        self.speed_spin.setValue(TRACK_INTERVAL_DEFAULT_US)
        self.speed_spin.setSingleStep(25)
        row.addWidget(self.speed_spin, 1)
        speed_l.addLayout(row)
        self.speed_btn = QtWidgets.QPushButton("下发 CMD 0x59 (RADAR_TRACK_SPEED)")
        self.speed_btn.clicked.connect(self._on_send_speed)
        speed_l.addWidget(self.speed_btn)
        if vis._transport != "ble":
            self.speed_spin.setEnabled(False)
            self.speed_btn.setEnabled(False)
            self.speed_btn.setToolTip("仅 BLE 模式可下发")
        right_l.addWidget(speed_box)

        conn_box = QtWidgets.QGroupBox("BLE 连接")
        conn_l = QtWidgets.QVBoxLayout(conn_box)
        row_addr = QtWidgets.QHBoxLayout()
        row_addr.addWidget(QtWidgets.QLabel("address:"))
        self.addr_edit = QtWidgets.QLineEdit()
        cur_addr, cur_en = self.vis.ble_target() if self.vis._transport == "ble" else ("", False)
        self.addr_edit.setText(cur_addr)
        row_addr.addWidget(self.addr_edit, 1)
        conn_l.addLayout(row_addr)
        self.conn_btn = QtWidgets.QPushButton("连接")
        self.conn_btn.setCheckable(True)
        self.conn_btn.setChecked(cur_en)
        self._update_conn_btn_text()
        self.conn_btn.clicked.connect(self._on_toggle_ble_conn)
        conn_l.addWidget(self.conn_btn)
        if vis._transport != "ble":
            self.addr_edit.setEnabled(False)
            self.conn_btn.setEnabled(False)
            self.conn_btn.setToolTip("仅 BLE 模式可用")
        right_l.addWidget(conn_box)

        svc_box = QtWidgets.QGroupBox("控制服务面板")
        svc_l = QtWidgets.QVBoxLayout(svc_box)
        svc_l.setSpacing(6)
        self._build_service_panel(svc_l)
        right_l.addWidget(svc_box, 1)

        self.ctrl_text = QtWidgets.QPlainTextEdit()
        self.ctrl_text.setReadOnly(True)
        self.ctrl_text.setMinimumHeight(130)
        self.ctrl_text.setPlaceholderText("Ctrl RSP / EVENT 日志")
        right_l.addWidget(self.ctrl_text, 1)

        splitter.addWidget(right)
        splitter.setSizes([720, 420])

        self.ax = self.figure.add_subplot(111, facecolor="#181825")
        self._setup_plot()

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(80)
        self.timer.timeout.connect(self._update_view)
        self.timer.start()

        self.log_timer = QtCore.QTimer(self)
        self.log_timer.setInterval(60)
        self.log_timer.timeout.connect(self._flush_raw_log)
        self.log_timer.start()

        self.ctrl_timer = QtCore.QTimer(self)
        self.ctrl_timer.setInterval(80)
        self.ctrl_timer.timeout.connect(self._flush_ctrl_log)
        self.ctrl_timer.start()

    def _on_send_speed(self) -> None:
        ok = self.vis.send_radar_track_interval_us(self.speed_spin.value())
        if not ok:
            self.radar_text.appendPlainText("[本地] 下发失败（非 BLE 或未连接）\n")

    def _update_conn_btn_text(self) -> None:
        self.conn_btn.setText("断开" if self.conn_btn.isChecked() else "连接")

    def _on_toggle_ble_conn(self) -> None:
        enabled = self.conn_btn.isChecked()
        addr = self.addr_edit.text().strip()
        if enabled and not addr:
            self.conn_btn.setChecked(False)
            self._update_conn_btn_text()
            self.radar_text.appendPlainText("[本地] 请输入 BLE address")
            return
        self.vis.set_ble_target(addr, enabled)
        self._update_conn_btn_text()
        self.radar_text.appendPlainText(f"[本地] BLE {'连接请求' if enabled else '已断开'}: {addr}")

    def _build_service_panel(self, root: QtWidgets.QVBoxLayout) -> None:
        g = QtWidgets.QGridLayout()
        g.setHorizontalSpacing(6)
        g.setVerticalSpacing(6)
        root.addLayout(g)

        # Power
        b_power_on = QtWidgets.QPushButton("PowerOn")
        b_power_on.clicked.connect(lambda: self._send(*vc.cmd_power_ctrl(1)))
        b_power_off = QtWidgets.QPushButton("PowerOff")
        b_power_off.clicked.connect(lambda: self._send(*vc.cmd_power_ctrl(0)))
        g.addWidget(QtWidgets.QLabel("电源 (0x12)"), 0, 0)
        g.addWidget(b_power_on, 0, 1)
        g.addWidget(b_power_off, 0, 2)

        # Direction control: press move, release stop
        g.addWidget(QtWidgets.QLabel("电机方向 (0x22, 按下动/松开停)"), 1, 0, 1, 4)
        self.dir_speed = QtWidgets.QSpinBox()
        self.dir_speed.setRange(0, 3)
        self.dir_speed.setValue(2)
        g.addWidget(QtWidgets.QLabel("speed"), 2, 0)
        g.addWidget(self.dir_speed, 2, 1)
        self.btn_up = QtWidgets.QPushButton("↑")
        self.btn_down = QtWidgets.QPushButton("↓")
        self.btn_left = QtWidgets.QPushButton("←")
        self.btn_right = QtWidgets.QPushButton("→")
        g.addWidget(self.btn_up, 3, 1)
        g.addWidget(self.btn_left, 4, 0)
        g.addWidget(self.btn_down, 4, 1)
        g.addWidget(self.btn_right, 4, 2)
        self._bind_press_release(self.btn_up, 0)
        self._bind_press_release(self.btn_down, 1)
        self._bind_press_release(self.btn_left, 2)
        self._bind_press_release(self.btn_right, 3)

        # Height
        g.addWidget(QtWidgets.QLabel("安装高度 (0x50, mm)"), 5, 0, 1, 2)
        self.h_mm = QtWidgets.QSpinBox()
        self.h_mm.setRange(500, 10000)
        self.h_mm.setValue(2500)
        b_h = QtWidgets.QPushButton("设置高度")
        b_h.clicked.connect(
            lambda: self._send(*vc.cmd_radar_set_height(self.h_mm.value()))
        )
        g.addWidget(self.h_mm, 6, 0)
        g.addWidget(b_h, 6, 1)

        # Boundary flow
        g.addWidget(QtWidgets.QLabel("边界流程 (0x51/52/53/55 + 0x56)"), 7, 0, 1, 5)
        self.b_idx = QtWidgets.QSpinBox()
        self.b_idx.setRange(0, 3)
        b_enter = QtWidgets.QPushButton("进入")
        b_sel = QtWidgets.QPushButton("选择点")
        b_save = QtWidgets.QPushButton("保存点")
        b_commit = QtWidgets.QPushButton("提交")
        b_reset = QtWidgets.QPushButton("复位")
        b_exit = QtWidgets.QPushButton("退出")
        b_enter.clicked.connect(lambda: self._send(*vc.cmd_radar_boundary_enter()))
        b_sel.clicked.connect(
            lambda: self._send(*vc.cmd_radar_boundary_select(self.b_idx.value()))
        )
        b_save.clicked.connect(
            lambda: self._send(*vc.cmd_radar_boundary_save(self.b_idx.value()))
        )
        b_commit.clicked.connect(lambda: self._send(*vc.cmd_radar_boundary_commit()))
        b_reset.clicked.connect(lambda: self._send(*vc.cmd_radar_reset_flash()))
        b_exit.clicked.connect(lambda: self._send(*vc.cmd_radar_boundary_exit()))
        g.addWidget(QtWidgets.QLabel("point"), 8, 0)
        g.addWidget(self.b_idx, 8, 1)
        g.addWidget(b_enter, 9, 0)
        g.addWidget(b_sel, 9, 1)
        g.addWidget(b_save, 9, 2)
        g.addWidget(b_commit, 9, 3)
        g.addWidget(b_reset, 9, 4)
        g.addWidget(b_exit, 9, 5)

    def _bind_press_release(self, btn: QtWidgets.QPushButton, direction: int) -> None:
        btn.pressed.connect(
            lambda d=direction: self._send(
                *vc.cmd_motor_dir_start(d, self.dir_speed.value())
            )
        )
        btn.released.connect(
            lambda d=direction: self._send(
                *vc.cmd_motor_dir_stop(d, self.dir_speed.value())
            )
        )

    def _send(self, cmd_id: int, payload: bytes, desc: str) -> None:
        ok = self.vis.send_cmd(cmd_id, payload, desc)
        if not ok:
            self.radar_text.appendPlainText("[本地] 下发失败（非 BLE 或未连接）")

    def _setup_plot(self):
        C_TEXT = "#cdd6f4"
        C_GRID = "#45475a"
        self.ax.tick_params(colors=C_TEXT)
        for s in self.ax.spines.values():
            s.set_color(C_GRID)
        self.ax.xaxis.label.set_color(C_TEXT)
        self.ax.yaxis.label.set_color(C_TEXT)
        self.ax.title.set_color("#89b4fa")

        with self.vis._lock:
            bq = [tuple(p) for p in self.vis.boundary_quad]
        quad = bq + [bq[0]]
        quad_xs = [p[0] for p in quad]
        quad_ys = [p[1] for p in quad]
        (self.boundary_line,) = self.ax.plot(
            quad_xs,
            quad_ys,
            color="#89b4fa",
            linewidth=2,
            alpha=0.95,
            label="Boundary",
        )

        (self.prev_point,) = self.ax.plot(
            [], [], marker="x", color="#f9e2af", markersize=9, mew=2, label="Prev"
        )
        (self.raw_point,) = self.ax.plot(
            [], [], "o", color="#89dceb", markersize=7, label="Raw"
        )
        (self.seq_points,) = self.ax.plot(
            [], [], ".", color="#f38ba8", alpha=0.85, markersize=8, label="Track / Seq"
        )

        self.raw_text = self.ax.text(
            0.02,
            0.98,
            "",
            transform=self.ax.transAxes,
            va="top",
            fontsize=9,
            color="#bac2de",
        )

        self.ax.set_xlabel("X (mm)", color=C_TEXT)
        self.ax.set_ylabel("Y (mm)", color=C_TEXT)
        self.ax.set_xlim(X_MIN - 100, X_MAX + 100)
        self.ax.set_ylim(Y_MIN - 100, Y_MAX + 100)
        self.ax.grid(True, linestyle="--", alpha=0.35, color=C_GRID)

        self.motion_arrow = FancyArrowPatch(
            (0.0, 0.0),
            (0.0, 0.0),
            arrowstyle="-|>",
            mutation_scale=28,
            linewidth=3.5,
            edgecolor="#f5c2e7",
            facecolor="#f5c2e7",
            zorder=9,
            label="Motion",
        )
        self.ax.add_patch(self.motion_arrow)
        self.motion_arrow.set_visible(False)

        leg = self.ax.legend(loc="lower right", facecolor="#1e1e2e", edgecolor=C_GRID)
        for t in leg.get_texts():
            t.set_color(C_TEXT)
        self.figure.tight_layout()

    def _flush_raw_log(self):
        with self.vis._lock:
            if not self.vis.log_lines:
                return
            lines = list(self.vis.log_lines)
            self.vis.log_lines.clear()
        self.log_text.appendPlainText("\n".join(lines))
        self.log_text.verticalScrollBar().setValue(
            self.log_text.verticalScrollBar().maximum()
        )

    def _flush_ctrl_log(self):
        with self.vis._lock:
            if not self.vis.ctrl_lines:
                return
            lines = list(self.vis.ctrl_lines)
            self.vis.ctrl_lines.clear()
        self.ctrl_text.appendPlainText("\n".join(lines))
        self.ctrl_text.verticalScrollBar().setValue(
            self.ctrl_text.verticalScrollBar().maximum()
        )

    def _update_view(self):
        with self.vis._lock:
            latest_prev = self.vis.latest_prev
            latest_raw = self.vis.latest_raw
            latest_a = self.vis.latest_pred_a
            latest_b = self.vis.latest_pred_b
            seq = list(self.vis.seq_history)
            bepoch = self.vis.boundary_epoch
            bquad = [tuple(p) for p in self.vis.boundary_quad]
            mdir_ok = self.vis.motion_dir_valid
            mdeg10 = self.vis.motion_dir_deg10

        if bepoch != self._last_boundary_epoch:
            self._last_boundary_epoch = bepoch
            q = bquad + [bquad[0]]
            self.boundary_line.set_data([p[0] for p in q], [p[1] for p in q])

        if latest_prev is not None:
            self.prev_point.set_data([latest_prev[0]], [latest_prev[1]])
        else:
            self.prev_point.set_data([], [])

        if latest_raw is not None:
            x, y, v = latest_raw
            self.raw_point.set_data([x], [y])
            if mdir_ok and v is None:
                th = mdeg10 / 10.0
                self.raw_text.set_text(f"RAW x={x} y={y} mm  θ={th:.1f}°")
            elif v is None:
                self.raw_text.set_text(f"RAW x={x} y={y} mm")
            else:
                self.raw_text.set_text(f"RAW x={x} y={y} mm  v={v} cm/s")
            if mdir_ok:
                rad = math.radians(mdeg10 / 10.0)
                ux = math.sin(rad)
                uy = math.cos(rad)
                x2 = float(x) + MOTION_ARROW_MM * ux
                y2 = float(y) + MOTION_ARROW_MM * uy
                self.motion_arrow.set_positions((float(x), float(y)), (x2, y2))
                self.motion_arrow.set_visible(True)
            else:
                self.motion_arrow.set_visible(False)
        else:
            self.raw_point.set_data([], [])
            self.raw_text.set_text("RAW: —")
            self.motion_arrow.set_visible(False)

        if seq:
            xs = [p[0] for p in seq]
            ys = [p[1] for p in seq]
            self.seq_points.set_data(xs, ys)
        else:
            self.seq_points.set_data([], [])

        lines = [
            "── 目标点 ──",
            f"Prev:     {latest_prev if latest_prev else '—'}",
            f"Raw:      {latest_raw[:2] if latest_raw else '—'}",
            f"Motion:   valid={mdir_ok}  dir_deg×10={mdeg10}",
            "",
            "── 预测 / 跟踪 ──",
            f"Seq pts:  {len(seq)}  {seq[-3:] if seq else ''}",
            "",
            "── 场地 ──",
            f"Boundary epoch: {bepoch}",
            f"Quad: {bquad}",
            "",
            f"UI 待下发 interval: {self.speed_spin.value()} µs  (CMD 0x59)",
        ]
        self.radar_text.setPlainText("\n".join(lines))

        self.canvas.draw_idle()


# python visializable.py --transport ble --address A4:C1:38:9F:96:BB
def main():
    parser = argparse.ArgumentParser(
        description="Radar prediction debug (serial or BLE)"
    )
    parser.add_argument(
        "--transport",
        choices=("ble", "serial"),
        default="ble",
        help="Data source (default: ble)",
    )
    parser.add_argument(
        "--port", default=PORT, help="Serial port when using --transport serial"
    )
    parser.add_argument("--baudrate", type=int, default=BAUDRATE)
    parser.add_argument(
        "--address",
        default="",
        help="BLE device address (Windows e.g. AA:BB:CC:DD:EE:FF). Required for BLE.",
    )
    parser.add_argument(
        "--ble-list-gatt",
        action="store_true",
        help="After connect, print full GATT table to stderr (debug).",
    )
    parser.add_argument(
        "--no-ble-request-boundary",
        action="store_true",
        help="Do not auto-write CMD 0x58 on connect (boundary only if you trigger manually).",
    )
    args = parser.parse_args()

    app = QtWidgets.QApplication(sys.argv)
    app.setStyleSheet(NIGHT_STYLESHEET)
    vis = RadarVisualizer(
        args.transport,
        port=args.port,
        baudrate=args.baudrate,
        ble_address=args.address or None,
        ble_list_gatt=args.ble_list_gatt,
        ble_request_boundary=(not args.no_ble_request_boundary),
    )
    vis.start()

    if args.transport == "ble":
        win_title = f"Radar visualizer (BLE {args.address or 'manual'})"
    else:
        win_title = f"Radar visualizer (serial {args.port})"

    main_window = RadarNightWindow(vis, win_title)
    main_window.show()

    exit_code = 0
    try:
        exit_code = app.exec_()
    finally:
        vis.close()
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
