## BLE 控制 Service 协议说明

本文件描述 **APP <-> BLE MCU** 的 GATT 控制通道协议（Ctrl RX/TX）。字段定义以 `vendor/ble_egg_anfu/app_ctrl.h` 为准。
### 设备名 SincereEGGA
### 1. GATT 结构

- Service UUID: `00 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- Ctrl RX(UUID): `01 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`（APP -> 设备，Write）
- Ctrl TX(UUID): `02 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`（设备 -> APP，Notify）
- 默认 MTU=23 时，**单帧限制**：`6 + payloadLen <= 20`

### 2. 通用帧格式

```
byte0 : version      固定 0x01
byte1 : msgType      0x01=CMD, 0x02=RSP, 0x03=EVENT
byte2 : cmdId
byte3 : seq          请求序号；响应需回显用于匹配
byte4 : payloadLen L
byte5 : payloadLen H
byte6.. : payload
```

### 3. RSP 状态码（payload[0]）

- `0x00` OK
- `0x01` LEN_ERROR
- `0x02` UNSUPPORTED_CMD
- `0x03` PARAM_ERROR
- `0x04` BUSY
- `0x05` STATE_CONFLICT
- `0x06` NO_OWNER_VOICE
- `0x07` STORAGE_ERROR
- `0x08` SOC_TIMEOUT
- `0x09` INTERNAL_ERROR

前端解析约定：
- 先判断 `payload[0]==0x00`，再解析后续字段。
- 响应的 `cmdId/seq` 必须与请求匹配。

### 4. CMD 清单（APP -> 设备）

#### 4.1 `0x13 STATUS_GET`（状态快照）

- **请求**：空
- **响应**：

`[status, powerState, workState, btLinked, ownerVoiceExist, volume, calmMode, enabledMask, usMask]`

- 字段：
  - `powerState`：0/1
  - `workState`：0=OFF，1=MONITORING，2=IDENTIFYING，3=ACTING，4=RESTING
  - `btLinked`：兼容字段，固定 1（可忽略）
  - `ownerVoiceExist`：0/1
  - `volume`：0~30
  - `calmMode`：0=自动，1=人工
  - `enabledMask`：bit0=音乐 bit1=主人录音 bit2=超声
  - `usMask`：bit0=25k bit1=30k bit2=25&30k

#### 4.2 `0x12 POWER_SET`（开关机）

- **请求**：`[onOff]`（0/1）
- **响应**：`[status, onOff]`

#### 4.3 `0x14 VOLUME_SET`（音量设置）

- **请求**：`[volume]`（0~30）
- **响应**：`[status, volumeApplied]`

#### 4.4 `0x15 VOLUME_GET`（音量读取）

- **请求**：空
- **响应**：`[status, volume]`

#### 4.5 主人录音相关（`0x20~0x25`）

1) `0x20 OWNER_REC_START`
- 请求：空
- 响应：`[status]`

2) `0x21 OWNER_REC_STOP`
- 请求：空
- 响应：`[status, durationSec]`（0~10；若 `<3s` 返回 `PARAM_ERROR` 且清文件）

3) `0x22 OWNER_REC_PLAY`
- 请求：空
- 响应：`[status]`（无录音返回 `NO_OWNER_VOICE`）

4) `0x25 OWNER_REC_PLAY_STOP`（停止播放，非暂停）
- 请求：空
- 响应：`[status]`（幂等）

5) `0x23 OWNER_REC_DELETE`
- 请求：空
- 响应：`[status]`（无文件可返回 `NO_OWNER_VOICE`，前端可视作已删）

6) `0x24 OWNER_REC_INFO_GET`
- 请求：空
- 响应：`[status, exist, durationSec]`

#### 4.6 `0x33 CALM_RECORD_GET`（安抚记录读取，兼容旧前端格式）

- **请求**：`[maxCount]`（1~16）
- **响应**：可能连续返回多包 RSP（每包 1 条记录）

`[status, count, index, startTs(u32 LE), endTs(u32 LE), tzQ15(s8)]`

- 字段：
  - `count`：本次返回的总条数（≤maxCount）
  - `index`：当前记录序号（0-based）
  - `endTs=0xFFFFFFFF`：表示该条仍在运行

前端用法：按 `index=0..count-1` 收齐即可渲染列表。

#### 4.7 安抚模式/策略

1) `0x30 CALM_MODE_SET`
- 请求：`[mode]`（0=自动，1=人工）
- 响应：`[status, modeApplied]`

2) `0x31 CALM_MODE_GET`
- 请求：空
- 响应：`[status, mode, effectiveUsOrder(3B)]`

3) `0x37 CALM_STRATEGY_SET`
- 请求：

`[mode, enabledMask, measureOrderCount, measureOrder..., usOrderCount, usOrder...]`

  - `measureOrder`：1=音乐 2=主人录音 3=超声
  - `usOrder`：1=25k 2=30k 3=25&30k
- 响应：`[status]`

4) `0x38 CALM_STRATEGY_GET`
- 请求：空
- 响应：

`[status, mode, enabledMask, measureOrderCount, measureOrder..., usOrderCount, usOrder...]`

#### 4.8 `0x32 TIME_SET`（时间同步）

- 请求：`[epochSec(u32 LE), tzQ15(s8)]`
- 响应：`[status]`

#### 4.9 `0x34 UID_GET`

- 请求：空
- 响应（2 包）：
  - `payload=[status, part(0/1), uid8bytes...]`（每包 8 字节 UID 分片）

#### 4.10 `0x50 FACTORY_RESET`（恢复出厂/清数据）

- 请求：`[reason]`（1=解绑触发；其它预留）
- 响应：`[status]`
- 语义：清主人录音、清安抚记录、清安抚配置并恢复默认。