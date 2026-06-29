# REASONIX — ble_egg_anfu (Telink B80 MCU)

蛋安抚器 BLE 主控固件，运行于 Telink B80（TC32 内核）。通过 UART 与 SigmaStar Star373 SOC（dog_soother）通信。

## Stack

- **MCU** Telink B80/B80B, TC32 RISC core
- **SDK** `tc_ble_simple_sdk_v3.4.2.2` (Telink BLE SDK)
- **Language** C (gnu99), assembly (startup)
- **Build** CMake 3.19+ → `tc32-elf-gcc` (TC32-GCC Toolchain)
- **BLE** Telink proprietary stack — controller + host (GAP/GATT/SMP) + HCI
- **Prebuilt libs** `lt_8208.a`, `firmware_encrypt.a`, `soft-fp.a` (`proj_lib/`)

## Layout

| Path | Contents |
|------|----------|
| `app.c` / `app.h` | BLE 协议栈初始化、广播、连接、主循环 |
| `app_att.c` / `app_att.h` | GATT service/characteristic 定义 |
| `app_uart.c` / `app_uart.h` | UART 帧协议：向 SOC 发 REQ、收 RSP/EVT |
| `app_ctrl.c` / `app_ctrl.h` | BLE 自定义 Service 控制协议（CMD/RSP/EVT） |
| `app_ui.c` / `app_ui.h` | 按键、OTA 等用户交互任务 |
| `app_config.h` | 功能开关（OTA/HID/deepsleep）和 Flash 分区 |
| `app_buffer.c` / `app_buffer.h` | ACL TX/RX FIFO, L2CAP RX Buffer 配置 |
| `main.c` | 入口 + IRQ handler |
| `tc32.c` / `tc32.h` | TC32 平台初始化（时钟、GPIO、中断向量） |
| `soc/star373/` | **子模块** — Star373 SDK + dog_soother 项目 |
| `docs/` | 接口设计文档 |
| `硬件/` | 硬件设计资料 |
| `scripts/` | 辅助脚本 |

## UART 协议（MCU ↔ SOC）

详见 `app_uart.h` + `soc/star373/…/dog_soother/include/uart_cmd.h`。

- **帧格式** `55 AA + len + msg_type + cmd_id + seq + payload + CRC16`
- **MCU→SOC**：`UART_MSG_REQ`（电源/音量/录音/安抚策略/时间同步/BLE连接通知）
- **SOC→MCU**：`UART_MSG_RSP`（响应）+ `UART_MSG_EVT`（工作状态/吠叫事件/录音完成/心跳）

## BLE 控制协议

详见 `app_ctrl.h` + `ctrl_service_doc.md`。

- 手机 APP 通过 BLE 自定义 Service 下发控制指令（`CTRL_CMD_xxx`）
- `app_ctrl_onRx()` — ATT write callback，解帧后路由到业务逻辑
- `app_ctrl_send()` — 向 APP 发 RSP/EVT
- `BLE_LOG_D()` 宏可将调试日志通过 BLE notify 发到 PC visualizer

## 构建

```bash
# 从 SDK 根目录:
cmake -B cmake_builds/tc_ble_simple_b80_sdk/TC32-GCC_Toolchain \
      -DPROJECT_NAME=tc_ble_simple_b80_sdk \
      -DTOOLCHAIN_NAME="TC32-GCC Toolchain" \
      -DTOOLCHAIN_PATH=<tc32_toolchain_path>
cmake --build cmake_builds/tc_ble_simple_b80_sdk/TC32-GCC_Toolchain \
      --target b80_ble_egg_anfu
```

产物: `cmake_builds/…/b80_ble_egg_anfu.bin`

## 注意事项

- **`vendor/ble_egg_anfu` 是 git 子模块** — 内部 commit 关联独立仓库
- **`config.h`** 选择 MCU 型号，`MCU_CORE_B80` / `MCU_CORE_B80B` 二选一
- **Flash 分区**：512K 设备，用户区 `0x40000–0x7C000`，雷达边界/高度/播放记录各占 4K sector
- **`BLE_APP_SECURITY_ENABLE` / `BLE_OTA_SERVER_ENABLE`** 等特性开关在 `app_config.h` 中关闭
- **深睡眠**：由 `PM_DEEPSLEEP_ENABLE` 控制，空闲 60s 后进睡眠
- **`.o` / `.d` 文件**是编译产物，勿手动编辑
