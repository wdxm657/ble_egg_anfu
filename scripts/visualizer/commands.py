from . import protocol as p


def cmd_power_ctrl(on: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_POWER_CTRL, bytes([1 if on else 0]), f"POWER_CTRL on={on}"


def cmd_motor_dir_start(direction: int, speed_lv: int = 2) -> tuple[int, bytes, str]:
    # payload: op=0x01(move), direction, speed
    return (
        p.CTRL_CMD_MOTOR_DIR_CTRL,
        bytes([0x01, direction & 0xFF, speed_lv & 0xFF]),
        f"MOTOR_DIR start dir={direction} speed={speed_lv}",
    )


def cmd_motor_dir_stop(direction: int = 0, speed_lv: int = 2) -> tuple[int, bytes, str]:
    # payload: op=0x00(stop), direction/speed are ignored by FW but keep frame shape
    return (
        p.CTRL_CMD_MOTOR_DIR_CTRL,
        bytes([0x00, direction & 0xFF, speed_lv & 0xFF]),
        "MOTOR_DIR stop",
    )


def cmd_radar_set_height(mm: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_SET_INSTALL_HEIGHT, p.s16le(mm), f"RADAR_SET_HEIGHT mm={mm}"


def cmd_radar_boundary_enter() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_ENTER, b"", "RADAR_BOUNDARY_ENTER"


def cmd_radar_boundary_select(idx: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_SELECT_POINT, bytes([idx & 0xFF]), f"RADAR_BOUNDARY_SELECT idx={idx}"


def cmd_radar_boundary_save(idx: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_SAVE_POINT, bytes([idx & 0xFF]), f"RADAR_BOUNDARY_SAVE idx={idx}"


def cmd_radar_boundary_commit() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_COMMIT, b"", "RADAR_BOUNDARY_COMMIT"


def cmd_radar_reset_flash() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_RESET_FLASH_CONFIG, b"", "RADAR_RESET_FLASH_CONFIG"


def cmd_motor_dir(op: int, direction: int, speed_lv: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_MOTOR_DIR_CTRL, bytes([op & 0xFF, direction & 0xFF, speed_lv & 0xFF]), f"MOTOR_DIR op={op} dir={direction} speed={speed_lv}"


def cmd_radar_set_height(mm: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_SET_INSTALL_HEIGHT, p.s16le(mm), f"RADAR_SET_HEIGHT mm={mm}"


def cmd_radar_boundary_enter() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_ENTER, b"", "RADAR_BOUNDARY_ENTER"


def cmd_radar_boundary_select(idx: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_SELECT_POINT, bytes([idx & 0xFF]), f"RADAR_BOUNDARY_SELECT idx={idx}"


def cmd_radar_boundary_save(idx: int) -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_SAVE_POINT, bytes([idx & 0xFF]), f"RADAR_BOUNDARY_SAVE idx={idx}"


def cmd_radar_boundary_commit() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_COMMIT, b"", "RADAR_BOUNDARY_COMMIT"


def cmd_radar_boundary_exit() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_BOUNDARY_EXIT, b"", "RADAR_BOUNDARY_EXIT"


def cmd_radar_reset_flash() -> tuple[int, bytes, str]:
    return p.CTRL_CMD_RADAR_RESET_FLASH_CONFIG, b"", "RADAR_RESET_FLASH_CONFIG"

