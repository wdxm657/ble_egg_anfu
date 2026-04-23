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
CTRL_CMD_CALM_MODE_SET = 0x30
CTRL_CMD_CALM_MODE_GET = 0x31
CTRL_CMD_TIME_SET = 0x32
CTRL_CMD_CALM_STRATEGY_SET = 0x33
CTRL_CMD_UID_GET = 0x34
CTRL_CMD_CALM_STRATEGY_GET = 0x35
CTRL_CMD_TEXT_CHUNK = 0x40

CMD_NAME = {
    CTRL_CMD_UNPAIR_REQ: "UNPAIR_REQ",
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
    CTRL_CMD_CALM_MODE_SET: "CALM_MODE_SET",
    CTRL_CMD_CALM_MODE_GET: "CALM_MODE_GET",
    CTRL_CMD_TIME_SET: "TIME_SET",
    CTRL_CMD_CALM_STRATEGY_SET: "CALM_STRATEGY_SET",
    CTRL_CMD_UID_GET: "UID_GET",
    CTRL_CMD_CALM_STRATEGY_GET: "CALM_STRATEGY_GET",
    CTRL_CMD_TEXT_CHUNK: "TEXT_CHUNK",
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

    def _decode_rsp(self, frame: CtrlFrame) -> str:
        status = frame.payload[0] if frame.payload else 0xFF
        base = f"[RSP] {CMD_NAME.get(frame.cmd_id, hex(frame.cmd_id))} status=0x{status:02X}"
        if frame.cmd_id == CTRL_CMD_STATUS_GET and len(frame.payload) >= 9:
            _, pwr, ws, bt, rec, vol, mode, em, um = frame.payload[:9]
            return f"{base} pwr={pwr} work={ws} bt={bt} rec={rec} vol={vol} mode={mode} enabled=0x{em:02X} us=0x{um:02X}"
        if frame.cmd_id == CTRL_CMD_VOLUME_GET and len(frame.payload) >= 2:
            return f"{base} volume={frame.payload[1]}"
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

                if measure_cnt > 3 or idx + measure_cnt + 1 > len(p):
                    return f"{base} [ERR] invalid measure section payload={p.hex()}"

                measure_order = list(p[idx : idx + measure_cnt])
                idx += measure_cnt

                us_cnt = p[idx]
                idx += 1
                if us_cnt > 3 or idx + us_cnt > len(p):
                    return f"{base} [ERR] invalid ultrasonic section payload={p.hex()}"

                us_order = list(p[idx : idx + us_cnt])

                def _measure_name(v: int) -> str:
                    return {1: "music", 2: "owner_voice", 3: "ultrasonic"}.get(v, f"unknown({v})")

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
        if frame.cmd_id == CTRL_CMD_UID_GET and len(frame.payload) >= 2:
            return f"{base} payload={frame.payload.hex()}"
        return f"{base} payload={frame.payload.hex()}"

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
        if frame.msg_type == CTRL_MSG_TYPE_RSP:
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
                    if not tx_char or not rx_char:
                        self._log("[BLE] Ctrl RX/TX 特征未找到")
                        continue

                    def _on_notify(_h, data: bytearray):
                        self._handle_notify(bytes(data))

                    await client.start_notify(tx_char, _on_notify)
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
                                f"[SEND] {desc} cmd={CMD_NAME.get(cmd_id, hex(cmd_id))} seq={seq} payload={payload.hex()}"
                            )
                        except queue.Empty:
                            pass
                        await asyncio.sleep(0.05)
                    self.connected = False
                    try:
                        await client.stop_notify(tx_char)
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
            (
                "解绑(恢复出厂)",
                lambda: self._send(CTRL_CMD_UNPAIR_REQ, b"", "UNPAIR_REQ"),
            ),
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
            ("试听播放", CTRL_CMD_OWNER_REC_PLAY),
            ("停止播放", CTRL_CMD_OWNER_REC_PLAY_STOP),
            ("删除录音", CTRL_CMD_OWNER_REC_DELETE),
            ("录音信息", CTRL_CMD_OWNER_REC_INFO_GET),
        ]
        for i, (txt, cmd) in enumerate(rec_btns):
            btn = QtWidgets.QPushButton(txt)
            btn.clicked.connect(lambda _=False, c=cmd, t=txt: self._send(c, b"", t))
            b2.addWidget(btn, i // 2, i % 2)
        grid.addWidget(box_rec, 0, 1)

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

        # 安抚策略
        box_strategy = QtWidgets.QGroupBox("安抚策略接口")
        b4 = QtWidgets.QGridLayout(box_strategy)
        self.chk_music = QtWidgets.QCheckBox("音乐")
        self.chk_owner = QtWidgets.QCheckBox("主人录音")
        self.chk_us = QtWidgets.QCheckBox("超声")
        self.chk_music.setChecked(True)
        self.chk_us.setChecked(True)
        self.ed_measure_order = QtWidgets.QLineEdit("1,3")
        self.ed_us_order = QtWidgets.QLineEdit("1,2,3")
        btn_strategy_set = QtWidgets.QPushButton("策略设置")
        btn_strategy_get = QtWidgets.QPushButton("策略查询")
        btn_strategy_set.clicked.connect(self._send_strategy_set)
        btn_strategy_get.clicked.connect(
            lambda: self._send(CTRL_CMD_CALM_STRATEGY_GET, b"", "CALM_STRATEGY_GET")
        )
        b4.addWidget(self.chk_music, 0, 0)
        b4.addWidget(self.chk_owner, 0, 1)
        b4.addWidget(self.chk_us, 0, 2)
        b4.addWidget(QtWidgets.QLabel("措施顺序(1,2,3):"), 1, 0)
        b4.addWidget(self.ed_measure_order, 1, 1, 1, 2)
        b4.addWidget(QtWidgets.QLabel("超声顺序(1,2,3):"), 2, 0)
        b4.addWidget(self.ed_us_order, 2, 1, 1, 2)
        b4.addWidget(btn_strategy_set, 3, 1)
        b4.addWidget(btn_strategy_get, 3, 2)
        # self.lbl_strategy_query = QtWidgets.QLabel("策略查询结果：-")
        # b4.addWidget(self.lbl_strategy_query, 4, 0, 1, 3)
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

    def _send_mode_set(self) -> None:
        mode = int(self.cmb_mode.currentData())
        self._send(CTRL_CMD_CALM_MODE_SET, bytes([mode]), f"CALM_MODE_SET mode={mode}")

    def _send_strategy_set(self) -> None:
        mode = int(self.cmb_mode.currentData())
        enabled = 0
        if self.chk_music.isChecked():
            enabled |= 1 << 0
        if self.chk_owner.isChecked():
            enabled |= 1 << 1
        if self.chk_us.isChecked():
            enabled |= 1 << 2
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
        if len(measure_order) > 3 or len(us_order) > 3:
            QtWidgets.QMessageBox.warning(
                self, "参数错误", "措施顺序和超声顺序最多 3 项"
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
        status = frame.payload[0]
        if status != 0x00:
            return

        if frame.cmd_id == CTRL_CMD_STATUS_GET and len(frame.payload) >= 9:
            _, pwr, ws, bt, rec, vol, mode, enabled_mask, us_mask = frame.payload[:9]
            self.sp_volume.setValue(int(vol))
            self._set_mode_combo(mode)
            self.chk_music.setChecked(bool(enabled_mask & 0x01))
            self.chk_owner.setChecked(bool(enabled_mask & 0x02))
            self.chk_us.setChecked(bool(enabled_mask & 0x04))
            # self.lbl_strategy_query.setText(
            #     f"策略镜像：enabledMask=0x{enabled_mask:02X} usMask=0x{us_mask:02X}"
            # )
            # self.lbl_status_query.setText(
            #     f"状态查询结果：pwr={pwr} work={ws} bt={bt} rec={rec} vol={vol} mode={mode} "
            #     f"enabledMask=0x{enabled_mask:02X} usMask=0x{us_mask:02X}"
            # )
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
            if m_cnt > 3 or idx + m_cnt + 1 > len(p):
                return
            measure_order = list(p[idx : idx + m_cnt])
            idx += m_cnt
            u_cnt = p[idx]
            idx += 1
            if u_cnt > 3 or idx + u_cnt > len(p):
                return
            us_order = list(p[idx : idx + u_cnt])

            self._set_mode_combo(mode)
            self.chk_music.setChecked(bool(enabled & 0x01))
            self.chk_owner.setChecked(bool(enabled & 0x02))
            self.chk_us.setChecked(bool(enabled & 0x04))
            self.ed_measure_order.setText(",".join(str(v) for v in measure_order))
            self.ed_us_order.setText(",".join(str(v) for v in us_order))
            # self.lbl_strategy_query.setText(
            #     f"策略查询结果：mode={mode} enabledMask=0x{enabled:02X} "
            #     f"measureOrder={measure_order} usOrder={us_order}"
            # )

    def _set_mode_combo(self, mode: int) -> None:
        idx = self.cmb_mode.findData(int(mode))
        if idx >= 0:
            self.cmb_mode.setCurrentIndex(idx)

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
