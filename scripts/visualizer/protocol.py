import struct
from dataclasses import dataclass
from typing import Optional

CTRL_PROTO_VERSION = 0x01

CTRL_MSG_TYPE_CMD = 0x01
CTRL_MSG_TYPE_RSP = 0x02
CTRL_MSG_TYPE_EVENT = 0x03

CTRL_CMD_LED_CTRL = 0x10
CTRL_CMD_LED_QUERY = 0x11
CTRL_CMD_POWER_CTRL = 0x12
CTRL_CMD_STATUS_GET = 0x13

CTRL_CMD_MOTOR_CTRL = 0x20
CTRL_CMD_MOTOR_SET_ZERO = 0x21
CTRL_CMD_MOTOR_DIR_CTRL = 0x22

CTRL_CMD_CFG_SET = 0x30
CTRL_CMD_CFG_GET = 0x31
CTRL_CMD_TIME_SET = 0x32
CTRL_CMD_PLAY_RECORD_GET = 0x33
CTRL_CMD_UID_GET = 0x34

CTRL_CMD_TEXT_CHUNK = 0x40

CTRL_CMD_RADAR_SET_INSTALL_HEIGHT = 0x50
CTRL_CMD_RADAR_BOUNDARY_ENTER = 0x51
CTRL_CMD_RADAR_BOUNDARY_SELECT_POINT = 0x52
CTRL_CMD_RADAR_BOUNDARY_SAVE_POINT = 0x53
CTRL_CMD_RADAR_BOUNDARY_EXIT = 0x54
CTRL_CMD_RADAR_BOUNDARY_COMMIT = 0x55
CTRL_CMD_RADAR_RESET_FLASH_CONFIG = 0x56
CTRL_CMD_RADAR_DEBUG_GET_BOUNDARY = 0x57
CTRL_CMD_RADAR_TRACK_SPEED = 0x58


@dataclass
class CtrlFrame:
    version: int
    msg_type: int
    cmd_id: int
    seq: int
    payload_len: int
    payload: bytes


def build_ctrl_cmd_frame(cmd_id: int, seq: int, payload: bytes) -> bytes:
    plen = len(payload)
    return bytes(
        [
            CTRL_PROTO_VERSION,
            CTRL_MSG_TYPE_CMD,
            cmd_id & 0xFF,
            seq & 0xFF,
            plen & 0xFF,
            (plen >> 8) & 0xFF,
        ]
    ) + payload


def parse_ctrl_frame(data: bytes) -> Optional[CtrlFrame]:
    if len(data) < 6:
        return None
    version = data[0]
    msg_type = data[1]
    cmd_id = data[2]
    seq = data[3]
    payload_len = data[4] | (data[5] << 8)
    if len(data) < 6 + payload_len:
        return None
    payload = data[6 : 6 + payload_len]
    return CtrlFrame(version, msg_type, cmd_id, seq, payload_len, payload)


def u16le(v: int) -> bytes:
    return struct.pack("<H", int(v) & 0xFFFF)


def s16le(v: int) -> bytes:
    return struct.pack("<h", int(v))

