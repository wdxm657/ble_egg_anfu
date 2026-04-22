## 通用控制 Service 协议说明（基于当前 `app_ctrl.c` 实现）

### 1. GATT 结构

- Service UUID: `00 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- Ctrl RX(UUID): `01 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`（APP -> 设备，Write）
- Ctrl TX(UUID): `02 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`（设备 -> APP，Notify）
- 当前实现按默认 MTU=23 设计，单帧总长度限制：`6 + payloadLen <= 20`

### 2. 通用帧格式

```
byte0 : version      协议版本，固定 0x01
byte1 : msgType      0x01=CMD, 0x02=RSP, 0x03=EVENT
byte2 : cmdId
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
byte6.. : payload
```

### 3. 错误码（响应 payload[0]）

- `0x00`：OK
- `0x01`：LEN_ERROR
- `0x02`：UNSUPPORTED_CMD
- `0x03`：PARAM_ERROR
- `0x04`：INTERNAL_ERROR

通用响应最小结构：

```
payload[0] = status
payload[1] = detail(当前一般为 0，部分命令有业务回显)
```

### 4. 当前已实现 CMD（仅 `app_ctrl_onRx` switch 中实际处理）

#### 4.1 设置时间（TIME_SET, `cmdId=0x32`）

- 请求 `payload` 长度必须为 5：
  - `payload[0..3]`：`epoch_sec`（u32，小端）
  - `payload[4]`：`tz_q15`（s8）
- 长度错误返回：`[PARAM_ERROR, 0x00]`
- 成功返回：`[OK, 0x00]`

请求示例：

```
01 01 32 01 05 00 80 D3 27 66 00
```

成功响应示例：

```
01 02 32 01 02 00 00 00
```

#### 4.2 状态查询（STATUS_GET, `cmdId=0x13`）

- 当前代码中已进入处理函数 `app_ctrl_handle_status_get()`。
- **现状**：函数当前仅 `return 0`，未发送响应帧。
- 请求推荐 `payloadLen=0`（当前实现未校验长度）。

请求示例：

```
01 01 13 01 00 00
```

#### 4.3 电源控制（POWER_CTRL, `cmdId=0x12`）

- 请求 `payload` 至少 1 字节：
  - `payload[0]`：开关值（非 0 视为开，0 为关）
- 长度不足返回：`[PARAM_ERROR, 0x00]`
- 成功时设备执行 `app_set_power_state(on)`，响应：`[OK, on]`

请求示例（开）：

```
01 01 12 01 01 00 01
```

成功响应示例（开）：

```
01 02 12 01 02 00 00 01
```

#### 4.4 读取 UID（UID_GET, `cmdId=0x34`）

- 请求必须 `payloadLen=0`，否则返回：`[PARAM_ERROR, 0x00]`
- 成功时读取 16 字节 UID，**分两帧响应**，每帧 10 字节 payload：
  - `payload[0]` = `OK`
  - `payload[1]` = `part`（0 或 1）
  - `payload[2..9]` = 对应 8 字节 UID 分片

请求示例：

```
01 01 34 01 00 00
```

响应示例（part=0）：

```
01 02 34 01 0A 00 00 00 xx xx xx xx xx xx xx xx
```

响应示例（part=1）：

```
01 02 34 01 0A 00 00 01 xx xx xx xx xx xx xx xx
```