import argparse
import asyncio
import datetime
import queue
import sys
import threading
import uuid
from dataclasses import dataclass
from typing import Dict, Optional, Set, Tuple

from bleak import BleakClient
from PyQt5 import QtCore, QtGui, QtWidgets

CTRL_PROTO_VERSION = 0x01
CTRL_MSG_TYPE_CMD = 0x01
CTRL_MSG_TYPE_RSP = 0x02
CTRL_MSG_TYPE_EVENT = 0x03

CTRL_CMD_UNPAIR_REQ = 0x02
CTRL_CMD_POWER_CTRL = 0x12
CTRL_CMD_STATUS_GET = 0x13
CTRL_CMD_VOLUME_SET = 0x14
CTRL_CMD_VOLUME_GET = 0x15
CTRL_CMD_OWNER_REC_START = 0x20
CTRL_CMD_OWNER_REC_STOP = 0x21
CTRL_CMD_OWNER_REC_PLAY = 0x22
CTRL_CMD_OWNER_REC_DELETE = 0x23
CTRL_CMD_OWNER_REC_INFO_GET = 0x24
CTRL_CMD_OWNER_REC_PLAY_STOP = 0x25
CTRL_CMD_OWNER_REC_SAVE = 0x26
CTRL_CMD_CALM_MUSIC_PLAY = 0x27
CTRL_CMD_CALM_MUSIC_PLAY_STOP = 0x28
CTRL_CMD_CALM_MODE_SET = 0x30
CTRL_CMD_CALM_MODE_GET = 0x31
CTRL_CMD_TIME_SET = 0x32
CTRL_CMD_CALM_RECORD_GET = 0x33
CTRL_CMD_CALM_RECORD_DELETE = 0x3A
CTRL_CMD_UID_GET = 0x34
CTRL_CMD_CALM_STRATEGY_SET = 0x37
CTRL_CMD_CALM_STRATEGY_GET = 0x38
CTRL_CMD_WORK_STATE_CHANGED = 0x80
CTRL_CMD_SOC_SESSION_START = 0x81
CTRL_CMD_SOC_MEASURE_EXEC = 0x82
CTRL_CMD_SOC_SESSION_RESULT = 0x83
CTRL_CMD_CALM_RECORD_NOTIFY = 0x84  # 安抚记录可用通知 event（设备→APP）
CTRL_CMD_SOC_ERROR = 0x86
CTRL_CMD_ULTRA_SET_25K = 0x60
CTRL_CMD_ULTRA_SET_30K = 0x61
CTRL_CMD_ULTRA_SET_DUAL = 0x62
CTRL_CMD_ULTRA_TRANS_ON = 0x63
CTRL_CMD_ULTRA_TRANS_OFF = 0x64
CTRL_CMD_ULTRA_EMIT_ON = 0x65
CTRL_CMD_ULTRA_EMIT_OFF = 0x66
CTRL_CMD_ULTRA_POWER = 0x67
CTRL_CMD_FACTORY_RESET = 0x50
CTRL_CMD_TEXT_CHUNK = 0x40

CMD_NAME = {
    CTRL_CMD_POWER_CTRL: "POWER_CTRL",
    CTRL_CMD_STATUS_GET: "STATUS_GET",
    CTRL_CMD_VOLUME_SET: "VOLUME_SET",
    CTRL_CMD_VOLUME_GET: "VOLUME_GET",
    CTRL_CMD_OWNER_REC_START: "OWNER_REC_START",
    CTRL_CMD_OWNER_REC_STOP: "OWNER_REC_STOP",
    CTRL_CMD_OWNER_REC_PLAY: "OWNER_REC_PLAY",
    CTRL_CMD_OWNER_REC_DELETE: "OWNER_REC_DELETE",
    CTRL_CMD_OWNER_REC_INFO_GET: "OWNER_REC_INFO_GET",
    CTRL_CMD_OWNER_REC_PLAY_STOP: "OWNER_REC_PLAY_STOP",
    CTRL_CMD_OWNER_REC_SAVE: "OWNER_REC_SAVE",
    CTRL_CMD_CALM_MODE_SET: "CALM_MODE_SET",
    CTRL_CMD_CALM_MODE_GET: "CALM_MODE_GET",
    CTRL_CMD_CALM_MUSIC_PLAY: "CALM_MUSIC_PLAY",
    CTRL_CMD_CALM_MUSIC_PLAY_STOP: "CALM_MUSIC_PLAY_STOP",
    CTRL_CMD_CALM_RECORD_GET: "CALM_RECORD_GET",
    CTRL_CMD_CALM_RECORD_DELETE: "CALM_RECORD_DELETE",
    CTRL_CMD_TIME_SET: "TIME_SET",
    CTRL_CMD_CALM_STRATEGY_SET: "CALM_STRATEGY_SET",
    CTRL_CMD_UID_GET: "UID_GET",
    CTRL_CMD_CALM_STRATEGY_GET: "CALM_STRATEGY_GET",
    CTRL_CMD_WORK_STATE_CHANGED: "WORK_STATE_CHANGED",
    CTRL_CMD_SOC_SESSION_START: "SOC_SESSION_START",
    CTRL_CMD_SOC_MEASURE_EXEC: "SOC_MEASURE_EXEC",
    CTRL_CMD_SOC_SESSION_RESULT: "SOC_SESSION_RESULT",
    CTRL_CMD_CALM_RECORD_NOTIFY: "CALM_RECORD_NOTIFY",
    CTRL_CMD_SOC_ERROR: "SOC_ERROR",
    CTRL_CMD_ULTRA_SET_25K: "ULTRA_SET_25K",
    CTRL_CMD_ULTRA_SET_30K: "ULTRA_SET_30K",
    CTRL_CMD_ULTRA_SET_DUAL: "ULTRA_SET_DUAL",
    CTRL_CMD_ULTRA_TRANS_ON: "ULTRA_TRANS_ON",
    CTRL_CMD_ULTRA_TRANS_OFF: "ULTRA_TRANS_OFF",
    CTRL_CMD_ULTRA_EMIT_ON: "ULTRA_EMIT_ON",
    CTRL_CMD_ULTRA_EMIT_OFF: "ULTRA_EMIT_OFF",
    CTRL_CMD_ULTRA_POWER: "ULTRA_POWER",
    CTRL_CMD_FACTORY_RESET: "FACTORY_RESET",
    CTRL_CMD_TEXT_CHUNK: "TEXT_CHUNK",
}

STATUS_NAME = {
    0x00: "OK",
    0x01: "LEN_ERROR",
    0x02: "UNSUPPORTED_CMD",
    0x03: "PARAM_ERROR",
    0x04: "INTERNAL_ERROR",
    0x05: "BUSY",
    0x06: "SOC_TIMEOUT",
    0x07: "SOC_ERROR"
}

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
CTRL_LOG_RAW_BYTES = bytes(
    [
        0x03,
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

# 标准 Battery Level 特征 UUID
BATTERY_LEVEL_UUID = "00002a19-0000-1000-8000-00805f9b34fb"


@dataclass
class CtrlFrame:
    version: int
    msg_type: int
    cmd_id: int
    seq: int
    payload_len: int
    payload: bytes


def parse_ctrl_frame(data: bytes) -> Optional[CtrlFrame]:
    if len(data) < 6:
        return None
    plen = data[4] | (data[5] << 8)
    if len(data) < 6 + plen:
        return None
    return CtrlFrame(data[0], data[1], data[2], data[3], plen, data[6 : 6 + plen])


def build_ctrl_cmd_frame(cmd_id: int, seq: int, payload: bytes) -> bytes:
    plen = len(payload)
    return (
        bytes(
            [
                CTRL_PROTO_VERSION,
                CTRL_MSG_TYPE_CMD,
                cmd_id & 0xFF,
                seq & 0xFF,
                plen & 0xFF,
                (plen >> 8) & 0xFF,
            ]
        )
        + payload
    )


def _uuid_candidates(raw: bytes) -> Set[int]:
    return {uuid.UUID(bytes=raw).int, uuid.UUID(bytes=raw[::-1]).int}


def _characteristic_uuid_int(char) -> Optional[int]:
    try:
        return uuid.UUID(str(char.uuid)).int
    except Exception:
        return None


def _find_char(client, candidates: Set[int]):
    for svc in client.services:
        for char in svc.characteristics:
            cid = _characteristic_uuid_int(char)
            if cid in candidates:
                return char
    return None


async def _bleak_ensure_gatt_ready(client) -> None:
    gs = getattr(client, "get_services", None)
    if gs and callable(gs):
        out = gs()
        if asyncio.iscoroutine(out):
            await out
    else:
        _ = client.services


class BleController:
    def __init__(self):
        self.lines = queue.Queue()
        self.rsp_frames: "queue.Queue[CtrlFrame]" = queue.Queue()
        self.cmd_q: "queue.Queue[Tuple[int, bytes, str]]" = queue.Queue()
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.address = ""
        self.connect_req = threading.Event()
        self.disconnect_req = threading.Event()
        self.connected = False
        self.seq = 0
        self._chunk_sessions: Dict[int, Dict[str, object]] = {}
        self._bat_percent = 0
        self._charging = 0

    def _log(self, line: str) -> None:
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        self.lines.put(f"[{ts}] {line}")

    def start(self) -> None:
        if self.thread and self.thread.is_alive():
            return
        self.thread = threading.Thread(target=self._run_loop, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.connect_req.set()
        self.disconnect_req.set()

    def request_connect(self, address: str) -> None:
        self.address = address.strip()
        self.connect_req.set()

    def request_disconnect(self) -> None:
        self.disconnect_req.set()

    def send_cmd(self, cmd_id: int, payload: bytes, desc: str) -> None:
        self.cmd_q.put((cmd_id, payload, desc))

    def _run_loop(self) -> None:
        asyncio.run(self._worker())

    SOC_EVENT_CMDS = {
        CTRL_CMD_WORK_STATE_CHANGED,
        CTRL_CMD_SOC_SESSION_START,
        CTRL_CMD_SOC_MEASURE_EXEC,
        CTRL_CMD_SOC_SESSION_RESULT,
        CTRL_CMD_CALM_RECORD_NOTIFY,
        CTRL_CMD_SOC_ERROR,
    }

    def _decode_rsp(self, frame: CtrlFrame) -> str:
        prefix = "[EVENT]" if frame.cmd_id in self.SOC_EVENT_CMDS else "[RSP]"
        name = CMD_NAME.get(frame.cmd_id, hex(frame.cmd_id))
        base = f"{prefix} {name}"

        # SOC events: payload 首字节不是 status，不显示 status 字段
        if frame.cmd_id in self.SOC_EVENT_CMDS and frame.payload:
            return self._decode_soc_event(frame, base)

        status = frame.payload[0] if frame.payload else 0xFF
        status_name = STATUS_NAME.get(status, "UNKNOWN")
        base = f"{base} status=0x{status:02X}({status_name})"
        if frame.cmd_id == CTRL_CMD_STATUS_GET and len(frame.payload) >= 9:
            _, pwr, ws, bt, vol, mode, em, um, chg = frame.payload[:9]
            chg_str = "充电中" if chg else ""
            return f"{base} pwr={pwr} work={ws} bt={bt} vol={vol} mode={mode} enabled=0x{em:02X} us=0x{um:02X} {chg_str}"
        if frame.cmd_id == CTRL_CMD_STATUS_GET and len(frame.payload) >= 8:
            _, pwr, ws, bt, vol, mode, em, um = frame.payload[:8]
            return f"{base} pwr={pwr} work={ws} bt={bt} vol={vol} mode={mode} enabled=0x{em:02X} us=0x{um:02X}"
        if frame.cmd_id == CTRL_CMD_VOLUME_GET and len(frame.payload) >= 2:
            return f"{base} volume={frame.payload[1]}"
        if frame.cmd_id == CTRL_CMD_OWNER_REC_PLAY and len(frame.payload) >= 1:
            src_name = ["saved", "tmp"]
            src = frame.payload[1] if len(frame.payload) >= 2 else 0
            return f"{base} src={src}({src_name[src] if src < len(src_name) else '?'})"
        if frame.cmd_id == CTRL_CMD_OWNER_REC_SAVE and len(frame.payload) >= 2:
            return f"{base} duration={frame.payload[1]}s"
        if frame.cmd_id == CTRL_CMD_OWNER_REC_INFO_GET and len(frame.payload) >= 3:
            return f"{base} exist={frame.payload[1]} duration={frame.payload[2]}s"
        if frame.cmd_id == CTRL_CMD_CALM_MODE_GET and len(frame.payload) >= 5:
            return f"{base} mode={frame.payload[1]} us_order={list(frame.payload[2:5])}"
        if frame.cmd_id == CTRL_CMD_CALM_STRATEGY_GET:
            p = frame.payload
            if len(p) >= 4 and p[0] == 0x00:
                idx = 1
                mode = p[idx]
                idx += 1
                enabled = p[idx]
                idx += 1
                measure_cnt = p[idx]
                idx += 1

                if measure_cnt > 4 or idx + measure_cnt + 1 > len(p):
                    return f"{base} [ERR] invalid measure section payload={p.hex()}"

                measure_order = list(p[idx : idx + measure_cnt])
                idx += measure_cnt

                us_cnt = p[idx]
                idx += 1
                if us_cnt > 3 or idx + us_cnt > len(p):
                    return f"{base} [ERR] invalid ultrasonic section payload={p.hex()}"

                us_order = list(p[idx : idx + us_cnt])

                def _measure_name(v: int) -> str:
                    return {1: "music", 2: "owner_voice", 3: "ultrasonic", 4: "snack_feed"}.get(v, f"unknown({v})")

                def _us_name(v: int) -> str:
                    return {1: "25k", 2: "30k", 3: "25k+30k"}.get(v, f"unknown({v})")

                measure_names = [_measure_name(v) for v in measure_order]
                us_names = [_us_name(v) for v in us_order]
                mode_name = "auto" if mode == 0 else ("manual" if mode == 1 else f"unknown({mode})")
                return (
                    f"{base} mode={mode}({mode_name}) enabledMask=0x{enabled:02X} "
                    f"measureOrder={measure_order}({measure_names}) "
                    f"usOrder={us_order}({us_names})"
                )
            return f"{base} payload={p.hex()}"
        if frame.cmd_id == CTRL_CMD_CALM_RECORD_GET:
            p = frame.payload
            # 新格式 12 字节: [status, entryIdx, totalEntries, session_id(4LE), type, ts(4)]
            if len(p) >= 12 and p[0] == 0x00:
                entry_idx = p[1]
                total_entries = p[2]
                session_id = int.from_bytes(p[3:7], "little", signed=False)
                entry_type = p[7]
                ts = int.from_bytes(p[8:12], "little", signed=False)

                type_names = {
                    0x01: "BARK",
                    0x02: "MUSIC",
                    0x03: "OWNER",
                    0x04: "US_25K",
                    0x05: "US_30K",
                    0x06: "US_DUAL",
                    0x07: "SNACK",
                    0x10: "SUCCESS",
                    0x11: "FAIL",
                }
                tname = type_names.get(entry_type, f"UNKNOWN(0x{entry_type:02X})")
                return (
                    f"{base} entry {entry_idx}/{total_entries} "
                    f"type=0x{entry_type:02X}({tname}) ts={ts} "
                    f"session_id={session_id}"
                )
            if len(p) == 1 and p[0] == 0x00:
                return f"{base} no records"
            return f"{base} payload={p.hex()}"
        if frame.cmd_id == CTRL_CMD_CALM_RECORD_DELETE:
            p = frame.payload
            if len(p) >= 1 and p[0] == 0x00:
                return f"{base} OK"
            return f"{base} status=0x{p[0]:02X}" if p else f"{base} payload empty"
        if frame.cmd_id == CTRL_CMD_UID_GET and len(frame.payload) >= 2:
            return f"{base} payload={frame.payload.hex()}"

        if frame.cmd_id == CTRL_CMD_SOC_ERROR and len(frame.payload) >= 1:
            return f"{base} errCode=0x{frame.payload[0]:02x}"

        return f"{base} payload={frame.payload.hex()}"

    def _decode_soc_event(self, frame: CtrlFrame, base: str) -> str:
        p = frame.payload
        if frame.cmd_id == CTRL_CMD_WORK_STATE_CHANGED and len(p) >= 2:
            ws = p[0]
            reason = p[1]
            ws_name = MainWindow.WORK_STATE_NAMES.get(ws, f"未知({ws})")
            reason_name = MainWindow.WORK_REASON_NAMES.get(reason, f"未知({reason})")
            return f"{base} workState={ws}({ws_name}) reason={reason}({reason_name})"

        if frame.cmd_id == CTRL_CMD_SOC_SESSION_START and len(p) >= 8:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            ts_val = int.from_bytes(p[4:8], "little", signed=False)
            return f"{base} session_id={sid} bark_ts={ts_val}"

        if frame.cmd_id == CTRL_CMD_SOC_MEASURE_EXEC and len(p) >= 11:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            step = p[4]
            measure = p[5]
            sub = p[6]
            ts_val = int.from_bytes(p[7:11], "little", signed=False)
            m_name = MainWindow.MEASURE_NAMES.get(measure, f"未知({measure})")
            if measure == 3:
                sub_name = MainWindow.US_SUB_NAMES.get(sub, f"未知({sub})")
                m_name += f"({sub_name})"
            return f"{base} session_id={sid} step={step} measure={measure}({m_name}) ts={ts_val}"

        if frame.cmd_id == CTRL_CMD_SOC_SESSION_RESULT and len(p) >= 11:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            result = p[4]
            ts_val = int.from_bytes(p[5:9], "little", signed=False)
            ok_measure = p[9]
            ok_sub = p[10]
            result_str = "成功" if result == 1 else "失败"
            m_name = MainWindow.MEASURE_NAMES.get(ok_measure, f"未知({ok_measure})")
            return f"{base} session_id={sid} result={result}({result_str}) ok_measure={ok_measure}({m_name}) ts={ts_val}"

        if frame.cmd_id == CTRL_CMD_CALM_RECORD_NOTIFY and len(p) >= 1:
            return f"{base} recordCount={p[0]}"

        return f"{base} payload={p.hex()}"

    def _handle_bat_notify(self, raw: bytes) -> None:
        """接收 Battery Level 特征通知"""
        if len(raw) >= 1:
            self._bat_percent = raw[0]

    def _handle_log_notify(self, raw: bytes) -> None:
        """接收 Log TX 特征通知，按行缓冲输出"""
        if not hasattr(self, '_log_buf'):
            self._log_buf = b''
        self._log_buf += raw
        while b'\n' in self._log_buf:
            line, self._log_buf = self._log_buf.split(b'\n', 1)
            line = line.rstrip(b'\r')
            if line:
                try:
                    text = line.decode("utf-8", errors="replace")
                    self._log(f"[LOG] {text}")
                except Exception:
                    self._log(f"[LOG][RAW] {line.hex()}")

    def _handle_notify(self, raw: bytes) -> None:
        frame = parse_ctrl_frame(raw)
        if not frame:
            self._log(f"[BLE][RAW] {raw.hex()}")
            return
        if (
            frame.msg_type == CTRL_MSG_TYPE_EVENT
            and frame.cmd_id == CTRL_CMD_TEXT_CHUNK
        ):
            payload = frame.payload
            if len(payload) < 4:
                self._log(f"[EVENT][TEXT_CHUNK][SHORT] {payload.hex()}")
                return
            transfer_id, chunk_index, chunk_total, data_len = payload[:4]
            if chunk_total == 0 or data_len > len(payload) - 4:
                self._log(f"[EVENT][TEXT_CHUNK][ERR] payload={payload.hex()}")
                return
            session = self._chunk_sessions.get(transfer_id)
            if session is None or session.get("total") != chunk_total:
                session = {"total": chunk_total, "parts": {}}
                self._chunk_sessions[transfer_id] = session
            parts = session["parts"]
            if isinstance(parts, dict):
                parts[chunk_index] = bytes(payload[4 : 4 + data_len])
                if len(parts) >= chunk_total:
                    merged = b"".join(parts.get(i, b"") for i in range(chunk_total))
                    text = merged.decode("utf-8", errors="ignore").strip()
                    if text:
                        self._log(f"[EVENT][TEXT] {text}")
                    self._chunk_sessions.pop(transfer_id, None)
            return
        # RSP 和 EVENT（除 TEXT_CHUNK 外）都放入 rsp_frames 队列供 UI 更新
        if frame.msg_type == CTRL_MSG_TYPE_RSP or frame.msg_type == CTRL_MSG_TYPE_EVENT:
            self.rsp_frames.put(frame)
            self._log(self._decode_rsp(frame))
            return
        self._log(
            f"[EVENT] cmd={CMD_NAME.get(frame.cmd_id, hex(frame.cmd_id))} payload={frame.payload.hex()}"
        )

    async def _worker(self) -> None:
        while not self.stop_event.is_set():
            self.connect_req.wait(0.2)
            if not self.connect_req.is_set():
                continue
            self.connect_req.clear()
            if not self.address:
                self._log("[BLE] 地址为空")
                continue
            try:
                self._log(f"[BLE] connecting {self.address}")
                async with BleakClient(self.address, timeout=15.0) as client:
                    await _bleak_ensure_gatt_ready(client)
                    tx_char = _find_char(client, _uuid_candidates(CTRL_TX_RAW_BYTES))
                    rx_char = _find_char(client, _uuid_candidates(CTRL_RX_RAW_BYTES))
                    log_char = _find_char(client, _uuid_candidates(CTRL_LOG_RAW_BYTES))
                    bat_char = None
                    for svc in client.services:
                        for ch in svc.characteristics:
                            if str(ch.uuid).lower() == BATTERY_LEVEL_UUID.lower():
                                bat_char = ch
                                break
                    if not tx_char or not rx_char:
                        self._log("[BLE] Ctrl RX/TX 特征未找到")
                        continue

                    def _on_notify(_h, data: bytearray):
                        self._handle_notify(bytes(data))

                    await client.start_notify(tx_char, _on_notify)

                    if log_char:
                        def _on_log_notify(_h, data: bytearray):
                            self._handle_log_notify(bytes(data))
                        await client.start_notify(log_char, _on_log_notify)
                        self._log("[BLE] Log TX 特征已订阅")
                    else:
                        self._log("[BLE] Log TX 特征未找到")

                    if bat_char:
                        def _on_bat_notify(_h, data: bytearray):
                            self._handle_bat_notify(bytes(data))
                        await client.start_notify(bat_char, _on_bat_notify)
                        self._log("[BLE] Battery 特征已订阅")
                    else:
                        self._log("[BLE] Battery 特征未找到")

                    self.connected = True
                    self._log("[BLE] connected")
                    while client.is_connected and not self.stop_event.is_set():
                        if self.disconnect_req.is_set():
                            self.disconnect_req.clear()
                            self._log("[BLE] disconnect requested")
                            break
                        try:
                            cmd_id, payload, desc = self.cmd_q.get_nowait()
                            seq = self.seq & 0xFF
                            self.seq = (self.seq + 1) & 0xFF
                            frame = build_ctrl_cmd_frame(cmd_id, seq, payload)
                            await client.write_gatt_char(rx_char, frame, response=False)
                            self._log(
                                f"[SEND] {desc} cmd={CMD_NAME.get(cmd_id, hex(cmd_id))} seq={seq} len={len(payload)} payload={payload.hex()}"
                            )
                        except queue.Empty:
                            pass
                        await asyncio.sleep(0.05)
                    self.connected = False
                    try:
                        await client.stop_notify(tx_char)
                        if log_char:
                            await client.stop_notify(log_char)
                    except Exception:
                        pass
                    self._log("[BLE] disconnected")
            except Exception as ex:
                self.connected = False
                self._log(f"[BLE] error: {ex}")


class MainWindow(QtWidgets.QWidget):
    def __init__(self, controller: BleController, default_addr: str):
        super().__init__()
        self.ctrl = controller
        self.setWindowTitle("BLE 控制台（蛋安抚器）")
        self.resize(1200, 760)
        self._setup_ui(default_addr)
        self._setup_timer()
        self._init_soothe_display()

    def _setup_ui(self, default_addr: str) -> None:
        root = QtWidgets.QVBoxLayout(self)
        conn = QtWidgets.QHBoxLayout()
        self.addr = QtWidgets.QLineEdit(default_addr)
        self.addr.setPlaceholderText("BLE 地址，例如 AA:BB:CC:DD:EE:FF")
        self.status = QtWidgets.QLabel("未连接")
        btn_conn = QtWidgets.QPushButton("连接")
        btn_disconn = QtWidgets.QPushButton("断开")
        btn_conn.clicked.connect(lambda: self.ctrl.request_connect(self.addr.text()))
        btn_disconn.clicked.connect(self.ctrl.request_disconnect)
        conn.addWidget(QtWidgets.QLabel("设备地址:"))
        conn.addWidget(self.addr, 1)
        conn.addWidget(btn_conn)
        conn.addWidget(btn_disconn)
        conn.addWidget(self.status)
        self.lbl_charging = QtWidgets.QLabel("")
        conn.addWidget(self.lbl_charging)
        root.addLayout(conn)

        grid = QtWidgets.QGridLayout()
        root.addLayout(grid)

        # 基础控制
        box_basic = QtWidgets.QGroupBox("基础接口")
        b1 = QtWidgets.QGridLayout(box_basic)
        self.sp_volume = QtWidgets.QSpinBox()
        self.sp_volume.setRange(0, 100)
        btns = [
            ("状态查询", lambda: self._send(CTRL_CMD_STATUS_GET, b"", "STATUS_GET")),
            ("开机", lambda: self._send(CTRL_CMD_POWER_CTRL, bytes([1]), "POWER ON")),
            ("关机", lambda: self._send(CTRL_CMD_POWER_CTRL, bytes([0]), "POWER OFF")),
            (
                "音量设置",
                lambda: self._send(
                    CTRL_CMD_VOLUME_SET, bytes([self.sp_volume.value()]), "VOLUME_SET"
                ),
            ),
            ("音量查询", lambda: self._send(CTRL_CMD_VOLUME_GET, b"", "VOLUME_GET")),
            ("恢复出厂(仅清数据)", lambda: self._send(CTRL_CMD_FACTORY_RESET, bytes([1]), "FACTORY_RESET reason=1")),
            ("时间同步(当前)", self._send_time_now),
            ("UID 查询", lambda: self._send(CTRL_CMD_UID_GET, b"", "UID_GET")),
        ]
        for i, (txt, fn) in enumerate(btns):
            btn = QtWidgets.QPushButton(txt)
            btn.clicked.connect(fn)
            b1.addWidget(btn, i // 2, (i % 2) * 2, 1, 2)
        # b1.addWidget(QtWidgets.QLabel("音量"), 1, 1)
        b1.addWidget(self.sp_volume, 1, 3)
        # self.lbl_status_query = QtWidgets.QLabel("状态查询结果：-")
        # b1.addWidget(self.lbl_status_query, 4, 0, 1, 4)
        grid.addWidget(box_basic, 0, 0)

        # 录音控制
        box_rec = QtWidgets.QGroupBox("主人录音接口")
        b2 = QtWidgets.QGridLayout(box_rec)
        rec_btns = [
            ("开始录制", CTRL_CMD_OWNER_REC_START),
            ("结束录制", CTRL_CMD_OWNER_REC_STOP),
            ("停止播放", CTRL_CMD_OWNER_REC_PLAY_STOP),
            ("删除录音", CTRL_CMD_OWNER_REC_DELETE),
            ("录音信息", CTRL_CMD_OWNER_REC_INFO_GET),
            ("保存音频", CTRL_CMD_OWNER_REC_SAVE),
        ]
        for i, (txt, cmd) in enumerate(rec_btns):
            btn = QtWidgets.QPushButton(txt)
            btn.clicked.connect(lambda _=False, c=cmd, t=txt: self._send(c, b"", t))
            b2.addWidget(btn, i // 2, i % 2)
        # 播放区域（带源选择）
        play_row = len(rec_btns) // 2 + (1 if len(rec_btns) % 2 else 0)
        b2.addWidget(QtWidgets.QLabel("播放源:"), play_row, 0)
        self.cmb_play_src = QtWidgets.QComboBox()
        self.cmb_play_src.addItem("已保存音频", 0)
        self.cmb_play_src.addItem("tmp 未保存", 1)
        b2.addWidget(self.cmb_play_src, play_row, 1)
        btn_play = QtWidgets.QPushButton("试听播放")
        btn_play.clicked.connect(self._send_owner_rec_play)
        b2.addWidget(btn_play, play_row, 2)
        grid.addWidget(box_rec, 0, 1)

        # 安抚音乐播放
        box_music = QtWidgets.QGroupBox("安抚音乐")
        bm = QtWidgets.QVBoxLayout(box_music)
        btn_music_play = QtWidgets.QPushButton("开始播放安抚音乐")
        btn_music_stop = QtWidgets.QPushButton("停止播放安抚音乐")
        btn_music_play.clicked.connect(
            lambda: self._send(CTRL_CMD_CALM_MUSIC_PLAY, b"", "CALM_MUSIC_PLAY")
        )
        btn_music_stop.clicked.connect(
            lambda: self._send(CTRL_CMD_CALM_MUSIC_PLAY_STOP, b"", "CALM_MUSIC_PLAY_STOP")
        )
        bm.addWidget(btn_music_play)
        bm.addWidget(btn_music_stop)
        grid.addWidget(box_music, 0, 2)

        # 超声波模块控制
        box_ultra = QtWidgets.QGroupBox("超声波模块")
        bu = QtWidgets.QGridLayout(box_ultra)
        ultra_btns = [
            ("25KHz",     CTRL_CMD_ULTRA_SET_25K),
            ("30KHz",     CTRL_CMD_ULTRA_SET_30K),
            ("双频",      CTRL_CMD_ULTRA_SET_DUAL),
            ("供电开",    CTRL_CMD_ULTRA_POWER, b"\x01"),
            ("供电关",    CTRL_CMD_ULTRA_POWER, b"\x00"),
            ("变压器开",  CTRL_CMD_ULTRA_TRANS_ON),
            ("变压器关",  CTRL_CMD_ULTRA_TRANS_OFF),
            ("发射",      CTRL_CMD_ULTRA_EMIT_ON),
            ("停止",      CTRL_CMD_ULTRA_EMIT_OFF),
        ]
        for i, item in enumerate(ultra_btns):
            txt, cmd = item[0], item[1]
            pl = item[2] if len(item) > 2 else b""
            btn = QtWidgets.QPushButton(txt)
            btn.clicked.connect(lambda _=False, c=cmd, p=pl, t=txt: self._send(c, p, t))
            bu.addWidget(btn, i // 3, i % 3)
        grid.addWidget(box_ultra, 0, 3)

        # 安抚模式
        box_mode = QtWidgets.QGroupBox("安抚模式接口")
        b3 = QtWidgets.QGridLayout(box_mode)
        self.cmb_mode = QtWidgets.QComboBox()
        self.cmb_mode.addItem("自动调整(0)", 0)
        self.cmb_mode.addItem("人工干预(1)", 1)
        btn_mode_set = QtWidgets.QPushButton("模式设置")
        btn_mode_get = QtWidgets.QPushButton("模式查询")
        btn_mode_set.clicked.connect(self._send_mode_set)
        btn_mode_get.clicked.connect(
            lambda: self._send(CTRL_CMD_CALM_MODE_GET, b"", "CALM_MODE_GET")
        )
        b3.addWidget(QtWidgets.QLabel("模式:"), 0, 0)
        b3.addWidget(self.cmb_mode, 0, 1)
        b3.addWidget(btn_mode_set, 1, 0)
        b3.addWidget(btn_mode_get, 1, 1)
        self.lbl_mode_query = QtWidgets.QLabel("模式查询结果：-")
        b3.addWidget(self.lbl_mode_query, 2, 0, 1, 2)
        grid.addWidget(box_mode, 1, 0)

        # 安抚过程实时显示
        box_soothe = QtWidgets.QGroupBox("安抚过程实时显示")
        bs = QtWidgets.QVBoxLayout(box_soothe)
        self.lbl_soothe_state = QtWidgets.QLabel("状态：等待中")
        self.lbl_soothe_session = QtWidgets.QLabel("会话：-")
        self.lbl_soothe_measure = QtWidgets.QLabel("当前措施：-")
        self.lbl_soothe_result = QtWidgets.QLabel("结果：-")
        self.lbl_soothe_state.setWordWrap(True)
        self.lbl_soothe_session.setWordWrap(True)
        self.lbl_soothe_measure.setWordWrap(True)
        self.lbl_soothe_result.setWordWrap(True)
        bs.addWidget(self.lbl_soothe_state)
        bs.addWidget(self.lbl_soothe_session)
        bs.addWidget(self.lbl_soothe_measure)
        bs.addWidget(self.lbl_soothe_result)
        grid.addWidget(box_soothe, 0, 4)

        # 安抚记录操作
        box_record = QtWidgets.QGroupBox("安抚记录操作")
        br = QtWidgets.QGridLayout(box_record)
        self.btn_record_get = QtWidgets.QPushButton("获取安抚记录")
        self.btn_record_del = QtWidgets.QPushButton("删除当前记录")
        self.btn_record_get.clicked.connect(self._send_record_get)
        self.btn_record_del.clicked.connect(self._send_record_del)
        self.lbl_record_info = QtWidgets.QLabel("记录信息：-")
        self.lbl_record_info.setWordWrap(True)
        self.txt_record_detail = QtWidgets.QTextEdit()
        self.txt_record_detail.setReadOnly(True)
        self.txt_record_detail.setMaximumHeight(120)
        br.addWidget(self.btn_record_get, 0, 0)
        br.addWidget(self.btn_record_del, 0, 1)
        br.addWidget(self.lbl_record_info, 1, 0, 1, 2)
        br.addWidget(self.txt_record_detail, 2, 0, 1, 2)
        grid.addWidget(box_record, 1, 2)

        self._init_record_state()

        # 安抚策略
        box_strategy = QtWidgets.QGroupBox("安抚策略接口")
        b4 = QtWidgets.QGridLayout(box_strategy)
        self.chk_music = QtWidgets.QCheckBox("音乐")
        self.chk_owner = QtWidgets.QCheckBox("主人录音")
        self.chk_us = QtWidgets.QCheckBox("超声")
        self.chk_snack = QtWidgets.QCheckBox("零食投喂")
        self.chk_music.setChecked(True)
        self.chk_us.setChecked(True)
        self.ed_measure_order = QtWidgets.QLineEdit("1,3")
        self.ed_us_order = QtWidgets.QLineEdit("1,2,3")
        self.cmb_strategy_mode = QtWidgets.QComboBox()
        self.cmb_strategy_mode.addItem("自动策略(0)", 0)
        self.cmb_strategy_mode.addItem("手动策略(1)", 1)
        btn_strategy_set = QtWidgets.QPushButton("策略设置")
        btn_strategy_get = QtWidgets.QPushButton("策略查询")
        btn_strategy_set.clicked.connect(self._send_strategy_set)
        btn_strategy_get.clicked.connect(self._send_strategy_get)
        b4.addWidget(self.chk_music, 0, 0)
        b4.addWidget(self.chk_owner, 0, 1)
        b4.addWidget(self.chk_us, 0, 2)
        b4.addWidget(self.chk_snack, 0, 3)
        b4.addWidget(QtWidgets.QLabel("措施顺序(1,2,3,4):"), 1, 0)
        b4.addWidget(self.ed_measure_order, 1, 1, 1, 3)
        b4.addWidget(QtWidgets.QLabel("超声顺序(1,2,3):"), 2, 0)
        b4.addWidget(self.ed_us_order, 2, 1, 1, 3)
        b4.addWidget(QtWidgets.QLabel("查询策略:"), 3, 0)
        b4.addWidget(self.cmb_strategy_mode, 3, 1)
        b4.addWidget(btn_strategy_set, 4, 1)
        b4.addWidget(btn_strategy_get, 4, 2)
        self.lbl_strategy_query = QtWidgets.QLabel("策略查询结果：-")
        b4.addWidget(self.lbl_strategy_query, 5, 0, 1, 3)
        grid.addWidget(box_strategy, 1, 1)

        self.log = QtWidgets.QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.document().setDefaultFont(QtGui.QFont("Consolas", 10))
        btn_clear = QtWidgets.QPushButton("清空日志")
        btn_clear.clicked.connect(self.log.clear)
        root.addWidget(btn_clear, 0)
        root.addWidget(self.log, 1)

    def _setup_timer(self) -> None:
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(80)
        self.timer.timeout.connect(self._flush_log)
        self.timer.start()

    def _send(self, cmd: int, payload: bytes, desc: str) -> None:
        self.ctrl.send_cmd(cmd, payload, desc)

    def _send_time_now(self) -> None:
        epoch = int(datetime.datetime.now().timestamp())
        tz_q15 = int(
            datetime.datetime.now().astimezone().utcoffset().total_seconds() // 900
        )
        payload = bytes(
            [
                epoch & 0xFF,
                (epoch >> 8) & 0xFF,
                (epoch >> 16) & 0xFF,
                (epoch >> 24) & 0xFF,
                tz_q15 & 0xFF,
            ]
        )
        self._send(
            CTRL_CMD_TIME_SET, payload, f"TIME_SET epoch={epoch} tz_q15={tz_q15}"
        )

    def _parse_csv_u8(self, txt: str) -> Optional[list]:
        try:
            if not txt.strip():
                return []
            arr = [int(x.strip()) for x in txt.split(",") if x.strip()]
            for v in arr:
                if v < 0 or v > 255:
                    return None
            return arr
        except Exception:
            return None

    def _send_owner_rec_play(self) -> None:
        src = int(self.cmb_play_src.currentData())
        self._send(CTRL_CMD_OWNER_REC_PLAY, bytes([src]), f"OWNER_REC_PLAY src={src}")

    def _send_mode_set(self) -> None:
        mode = int(self.cmb_mode.currentData())
        self._send(CTRL_CMD_CALM_MODE_SET, bytes([mode]), f"CALM_MODE_SET mode={mode}")

    def _send_strategy_set(self) -> None:
        mode = int(self.cmb_strategy_mode.currentData())
        enabled = 0
        if self.chk_music.isChecked():
            enabled |= 1 << 0
        if self.chk_owner.isChecked():
            enabled |= 1 << 1
        if self.chk_us.isChecked():
            enabled |= 1 << 2
        if self.chk_snack.isChecked():
            enabled |= 1 << 3
        if enabled == 0:
            QtWidgets.QMessageBox.warning(self, "参数错误", "至少勾选一种安抚措施")
            return

        measure_order = self._parse_csv_u8(self.ed_measure_order.text())
        us_order = self._parse_csv_u8(self.ed_us_order.text())
        if measure_order is None or us_order is None:
            QtWidgets.QMessageBox.warning(
                self, "参数错误", "顺序输入格式错误，请用逗号分隔整数"
            )
            return
        if mode == 0 and ((enabled & (1 << 3)) or 4 in measure_order):
            QtWidgets.QMessageBox.warning(
                self, "参数错误", "自动策略禁止配置零食投喂"
            )
            return
        if len(measure_order) > 4 or len(us_order) > 3:
            QtWidgets.QMessageBox.warning(
                self, "参数错误", "措施顺序最多 4 项，超声顺序最多 3 项"
            )
            return

        payload = bytes(
            [mode, enabled, len(measure_order)]
            + measure_order
            + [len(us_order)]
            + us_order
        )
        self._send(CTRL_CMD_CALM_STRATEGY_SET, payload, "CALM_STRATEGY_SET")

    def _flush_log(self) -> None:
        self._flush_rsp_frames()
        lines = []
        while True:
            try:
                lines.append(self.ctrl.lines.get_nowait())
            except queue.Empty:
                break
        if lines:
            self.log.appendPlainText("\n".join(lines))
            self.log.verticalScrollBar().setValue(
                self.log.verticalScrollBar().maximum()
            )
        self.status.setText("已连接" if self.ctrl.connected else "未连接")
        # 每 tick 刷新电池显示
        bat = getattr(self.ctrl, '_bat_percent', 0)
        chg = getattr(self.ctrl, '_charging', 0)
        icon = self._battery_icon(bat, chg)
        self.lbl_charging.setText(f"{icon} {bat}%")
        if chg:
            self.lbl_charging.setStyleSheet("color: #00FF00; font-weight: bold;")
        else:
            self.lbl_charging.setStyleSheet("color: #FFFFFF;")

    def _flush_rsp_frames(self) -> None:
        while True:
            try:
                frame = self.ctrl.rsp_frames.get_nowait()
            except queue.Empty:
                break
            self._apply_rsp_to_ui(frame)

    def _apply_rsp_to_ui(self, frame: CtrlFrame) -> None:
        if not frame.payload:
            return

        # SOC 事件：payload 首字节不是 status，直接处理
        if frame.cmd_id in (
            CTRL_CMD_WORK_STATE_CHANGED,
            CTRL_CMD_SOC_SESSION_START,
            CTRL_CMD_SOC_MEASURE_EXEC,
            CTRL_CMD_SOC_SESSION_RESULT,
        ):
            if frame.cmd_id == CTRL_CMD_WORK_STATE_CHANGED:
                self._apply_soothe_work_state(frame)
            elif frame.cmd_id == CTRL_CMD_SOC_SESSION_START:
                self._apply_soothe_session_start(frame)
            elif frame.cmd_id == CTRL_CMD_SOC_MEASURE_EXEC:
                self._apply_soothe_measure_exec(frame)
            elif frame.cmd_id == CTRL_CMD_SOC_SESSION_RESULT:
                self._apply_soothe_session_result(frame)
            return

        status = frame.payload[0]
        if status != 0x00:
            return

        if frame.cmd_id == CTRL_CMD_STATUS_GET and len(frame.payload) >= 8:
            _, pwr, ws, bt, vol, mode, enabled_mask, us_mask = frame.payload[:8]
            self.sp_volume.setValue(int(vol))
            self._set_mode_combo(mode)
            self.chk_music.setChecked(bool(enabled_mask & 0x01))
            self.chk_owner.setChecked(bool(enabled_mask & 0x02))
            self.chk_us.setChecked(bool(enabled_mask & 0x04))
            self.chk_snack.setChecked(bool(enabled_mask & 0x08))
            # 充电状态（byte8），电池百分比来自标准 Battery Service
            chg = frame.payload[8] if len(frame.payload) >= 9 else 0
            self.ctrl._charging = chg
            bat = getattr(self.ctrl, '_bat_percent', 0)
            icon = self._battery_icon(bat, chg)
            self.lbl_charging.setText(f"{icon} {bat}%")
            if chg:
                self.lbl_charging.setStyleSheet("color: #00FF00; font-weight: bold;")
            else:
                self.lbl_charging.setStyleSheet("color: #FFFFFF;")
            return

        if frame.cmd_id == CTRL_CMD_CALM_MODE_GET and len(frame.payload) >= 2:
            mode = frame.payload[1]
            self._set_mode_combo(mode)
            self.lbl_mode_query.setText(f"模式查询结果：mode={mode}")
            return

        if frame.cmd_id == CTRL_CMD_CALM_STRATEGY_GET and len(frame.payload) >= 4:
            p = frame.payload
            idx = 1
            mode = p[idx]
            idx += 1
            enabled = p[idx]
            idx += 1
            m_cnt = p[idx]
            idx += 1
            if m_cnt > 4 or idx + m_cnt + 1 > len(p):
                return
            measure_order = list(p[idx : idx + m_cnt])
            idx += m_cnt
            u_cnt = p[idx]
            idx += 1
            if u_cnt > 3 or idx + u_cnt > len(p):
                return
            us_order = list(p[idx : idx + u_cnt])

            self._set_mode_combo(mode)
            self._set_strategy_mode_combo(mode)
            self.chk_music.setChecked(bool(enabled & 0x01))
            self.chk_owner.setChecked(bool(enabled & 0x02))
            self.chk_us.setChecked(bool(enabled & 0x04))
            self.chk_snack.setChecked(bool(enabled & 0x08))
            self.ed_measure_order.setText(",".join(str(v) for v in measure_order))
            self.ed_us_order.setText(",".join(str(v) for v in us_order))
            measure_names = [self._measure_name(v) for v in measure_order]
            us_names = [self._us_name(v) for v in us_order]
            mode_name = "自动" if mode == 0 else ("手动" if mode == 1 else f"未知({mode})")
            self.lbl_strategy_query.setText(
                f"策略查询结果({mode_name})："
                f"enabledMask=0x{enabled:02X} "
                f"measureOrder={measure_order}{measure_names} "
                f"usOrder={us_order}{us_names}"
            )
            return

        if frame.cmd_id == CTRL_CMD_CALM_RECORD_GET:
            self._apply_record_rsp(frame)
            return

        if frame.cmd_id == CTRL_CMD_CALM_RECORD_DELETE:
            self._apply_record_del_rsp(frame)
            return

    def _init_soothe_display(self) -> None:
        self._soothe_session_id = 0
        self._soothe_measure_idx = 0
        self._soothe_result_pending = False

    WORK_STATE_NAMES = {
        0: "OFF",
        1: "监测中",
        2: "识别中",
        3: "执行中",
        4: "休息中",
    }

    WORK_REASON_NAMES = {
        0: "power_on",
        1: "bark_detected",
        2: "acting",
        3: "success_rest",
        4: "fail_rest",
        5: "rest_done",
        6: "post_measure",
        7: "identify_timeout",
        8: "post_measure_bark",
    }

    MEASURE_NAMES = {
        1: "音乐",
        2: "主人录音",
        3: "超声",
        4: "零食投喂",
    }

    US_SUB_NAMES = {
        1: "25KHz",
        2: "30KHz",
        3: "双频",
    }

    def _apply_soothe_work_state(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) >= 2:
            ws = p[0]
            reason = p[1]
            ws_name = self.WORK_STATE_NAMES.get(ws, f"未知({ws})")
            reason_name = self.WORK_REASON_NAMES.get(reason, f"未知({reason})")
            self.lbl_soothe_state.setText(f"状态：{ws_name} (reason={reason_name})")
            if ws == 4:  # RESTING — 安抚结束
                self.lbl_soothe_measure.setText("当前措施：- (会话结束)")

    def _apply_soothe_session_start(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) >= 8:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            ts = int.from_bytes(p[4:8], "little", signed=False)
            self._soothe_session_id = sid
            self._soothe_measure_idx = 0
            self._soothe_result_pending = True
            self.lbl_soothe_session.setText(f"会话：#{sid} 开始 @{ts}")
            self.lbl_soothe_measure.setText("当前措施：准备执行...")
            self.lbl_soothe_result.setText("结果：等待中...")

    def _apply_soothe_measure_exec(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) >= 11:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            step = p[4]
            measure = p[5]
            sub = p[6]
            m_name = self.MEASURE_NAMES.get(measure, f"未知({measure})")
            if measure == 3 and sub in self.US_SUB_NAMES:
                m_name += f" ({self.US_SUB_NAMES[sub]})"
            self._soothe_measure_idx = step
            self.lbl_soothe_measure.setText(f"当前措施：第{step+1}步 — {m_name}")

    def _apply_soothe_session_result(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) >= 11:
            sid = int.from_bytes(p[0:4], "little", signed=False)
            result = p[4]
            ok_measure = p[9]
            result_str = "✅ 成功" if result == 1 else "❌ 失败"
            m_name = self.MEASURE_NAMES.get(ok_measure, f"未知({ok_measure})")
            self._soothe_result_pending = False
            self.lbl_soothe_result.setText(f"结果：{result_str} (有效措施={m_name})")

    def _init_record_state(self) -> None:
        self._rec_entries = []       # 当前记录的 entry 列表 [(type, ts_str), ...]
        self._rec_session_id = 0     # 当前记录的 session_id（用于删除）
        self._rec_total = 0          # 本条记录总 entry 数
        self._rec_received = 0       # 已收到 entry 数

    def _send_record_get(self) -> None:
        self._init_record_state()
        self.txt_record_detail.clear()
        self.lbl_record_info.setText("正在获取安抚记录...")
        self._send(CTRL_CMD_CALM_RECORD_GET, b"", "CALM_RECORD_GET")

    def _send_record_del(self) -> None:
        sid = getattr(self, "_rec_session_id", 0)
        payload = bytes([sid & 0xFF])
        self._send(CTRL_CMD_CALM_RECORD_DELETE, payload, f"CALM_RECORD_DELETE session_id={sid}")

    @staticmethod
    def _record_type_name(t: int) -> str:
        names = {
            0x01: "BARK",
            0x02: "MUSIC",
            0x03: "OWNER",
            0x04: "US_25K",
            0x05: "US_30K",
            0x06: "US_DUAL",
            0x07: "SNACK",
            0x10: "SUCCESS",
            0x11: "FAIL",
        }
        return names.get(t, f"UNKNOWN(0x{t:02X})")

    @staticmethod
    def _battery_icon(percent: int, charging: bool) -> str:
        if charging:
            return "🔌"
        if percent >= 80:
            return "🟢"
        if percent >= 30:
            return "🟡"
        return "🔴"

    def _set_mode_combo(self, mode: int) -> None:
        idx = self.cmb_mode.findData(int(mode))
        if idx >= 0:
            self.cmb_mode.setCurrentIndex(idx)

    def _set_strategy_mode_combo(self, mode: int) -> None:
        idx = self.cmb_strategy_mode.findData(int(mode))
        if idx >= 0:
            self.cmb_strategy_mode.setCurrentIndex(idx)

    def _apply_record_rsp(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) == 1 and p[0] == 0x00:
            self.lbl_record_info.setText("记录信息：无更多安抚记录")
            self.txt_record_detail.clear()
            return

        # 格式 9 字节: [status, entryIdx, totalEntries, session_id(1), type, ts(4)]
        if len(p) >= 9 and p[0] == 0x00:
            entry_idx = p[1]
            total_entries = p[2]
            session_id = p[3]
            entry_type = p[4]
            ts = int.from_bytes(p[5:9], "little", signed=False)
            ts_str = datetime.datetime.fromtimestamp(ts).strftime("%H:%M:%S")

            if entry_idx == 0:
                self._init_record_state()
                self._rec_total = total_entries
                self._rec_session_id = session_id
                self.txt_record_detail.clear()

            self._rec_received = entry_idx + 1
            type_name = self._record_type_name(entry_type)
            self._rec_entries.append((type_name, ts_str))

            # 更新显示
            lines = []
            for i, (tn, ts_s) in enumerate(self._rec_entries):
                icon = "🐶" if tn == "BARK" else (
                       "✅" if tn == "SUCCESS" else (
                       "❌" if tn == "FAIL" else (
                       "🎵" if tn in ("MUSIC", "OWNER") else "📡")))
                lines.append(f"  {icon} [{i}] {tn}  @{ts_s}")
            self.txt_record_detail.setText("\n".join(lines))

            info = f"记录信息：第 {entry_idx+1}/{total_entries} 条, session_id={session_id}"
            if entry_idx + 1 >= total_entries:
                info += " (记录完整)"
            self.lbl_record_info.setText(info)
            return

        self.lbl_record_info.setText(f"记录信息：异常响应 payload={p.hex()}")

    def _apply_record_del_rsp(self, frame: CtrlFrame) -> None:
        p = frame.payload
        if len(p) >= 1 and p[0] == 0x00:
            self.lbl_record_info.setText("记录信息：已删除")
            self.txt_record_detail.clear()
            self._init_record_state()
            return
        self.lbl_record_info.setText("记录信息：删除失败")

    def _send_strategy_get(self) -> None:
        mode = int(self.cmb_strategy_mode.currentData())
        payload = bytes([mode])
        self._send(CTRL_CMD_CALM_STRATEGY_GET, payload, f"CALM_STRATEGY_GET mode={mode}")

    @staticmethod
    def _measure_name(v: int) -> str:
        return {1: "music", 2: "owner_voice", 3: "ultrasonic", 4: "snack_feed"}.get(v, f"unknown({v})")

    @staticmethod
    def _us_name(v: int) -> str:
        return {1: "25k", 2: "30k", 3: "25k+30k"}.get(v, f"unknown({v})")

    def closeEvent(self, event) -> None:
        self.ctrl.stop()
        super().closeEvent(event)


def apply_dark_theme(app: QtWidgets.QApplication) -> None:
    app.setStyle("Fusion")
    palette = QtGui.QPalette()
    palette.setColor(QtGui.QPalette.Window, QtGui.QColor(36, 38, 44))
    palette.setColor(QtGui.QPalette.WindowText, QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Base, QtGui.QColor(28, 30, 34))
    palette.setColor(QtGui.QPalette.AlternateBase, QtGui.QColor(45, 48, 56))
    palette.setColor(QtGui.QPalette.ToolTipBase, QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.ToolTipText, QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Text, QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Button, QtGui.QColor(58, 62, 72))
    palette.setColor(QtGui.QPalette.ButtonText, QtGui.QColor(220, 220, 220))
    palette.setColor(QtGui.QPalette.Highlight, QtGui.QColor(90, 140, 255))
    palette.setColor(QtGui.QPalette.HighlightedText, QtGui.QColor(240, 240, 240))
    app.setPalette(palette)


def parse_args():
    parser = argparse.ArgumentParser(description="BLE 可视化控制工具（PyQt 夜间模式）")
    parser.add_argument(
        "--ble-address", default="", help="BLE MAC 地址，可在界面里再修改"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    app = QtWidgets.QApplication(sys.argv)
    apply_dark_theme(app)
    controller = BleController()
    controller.start()
    win = MainWindow(controller, args.ble_address)
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
