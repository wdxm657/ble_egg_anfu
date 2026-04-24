## 通用控制 Service 协议说明（给 APP 开发）

### 蓝牙名称 SincereEGGA

### 1. GATT 结构概览

- **Service：Custom Control Service**
- UUID（16 字节）：`00 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- **Characteristic 1：Ctrl RX（APP -> 设备）**
  - UUID：`01 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
  - 属性：Read | Write | Write Without Response
  - 用途：APP 向设备发送所有控制命令
- **Characteristic 2：Ctrl TX（设备 -> APP）**
  - UUID：`02 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
  - 属性：Read | Notify
  - 需先向 CCC 写 `0x0001` 开启 Notify
  - 用途：设备向 APP 返回响应/事件

当前固件按默认 MTU=23 设计，单帧总长度限制：`6 + payloadLen <= 20`。

---

### 2. 通用帧格式（Ctrl RX / Ctrl TX 共用）

```
byte0 : version      协议版本，固定 0x01
byte1 : msgType      0x01=CMD, 0x02=RSP, 0x03=EVENT
byte2 : cmdId        命令 ID
byte3 : seq          请求序号（响应原样回显）
byte4 : payloadLen L 负载长度低字节
byte5 : payloadLen H 负载长度高字节
byte6.. : payload    具体负载，长度 = payloadLen
```

---

### 3. 错误码（响应 payload[0]）

- `0x00`：OK
- `0x01`：LEN_ERROR
- `0x02`：UNSUPPORTED_CMD
- `0x03`：PARAM_ERROR
- `0x04`：INTERNAL_ERROR
- `0x05`：BUSY
- `0x06`：STATE_CONFLICT
- `0x07`：NO_OWNER_VOICE
- `0x08`：STORAGE_ERROR
- `0x09`：SOC_TIMEOUT

---

### 4. 已实现命令列表及完整帧结构

#### 4.1 电源开关（POWER_CTRL，CMD = 0x12）

**请求帧（APP -> 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x12           // cmdId = POWER_CTRL
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : onOff          // 0x00=关, 0x01=开
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x12           // cmdId = POWER_CTRL
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : onOff          // 回显应用值
```

#### 4.2 状态查询（STATUS_GET，CMD = 0x13）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x13           // cmdId = STATUS_GET
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x13           // cmdId = STATUS_GET
byte3 : seq
byte4 : 0x09           // payloadLen = 9
byte5 : 0x00
byte6 : status
byte7 : powerState
byte8 : workState
byte9 : btLinked
byte10: ownerVoiceExist
byte11: volume
byte12: calmMode
byte13: enabledMask
byte14: usMask
```

#### 4.3 音量设置（VOLUME_SET，CMD = 0x14）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x14
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : volume         // 0~30
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x14
byte3 : seq
byte4 : 0x02
byte5 : 0x00
byte6 : status
byte7 : volumeApplied
```

#### 4.4 音量查询（VOLUME_GET，CMD = 0x15）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x15
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x15
byte3 : seq
byte4 : 0x02
byte5 : 0x00
byte6 : status
byte7 : volume
```

#### 4.5 主人录音开始（OWNER_REC_START，CMD = 0x20）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x20
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x20
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.6 主人录音结束（OWNER_REC_STOP，CMD = 0x21）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x21
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x21
byte3 : seq
byte4 : 0x02
byte5 : 0x00
byte6 : status
byte7 : durationSec
```

#### 4.7 主人录音播放（OWNER_REC_PLAY，CMD = 0x22）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x22
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x22
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.8 主人录音停止播放（OWNER_REC_PLAY_STOP，CMD = 0x25）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x25
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x25
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.9 主人录音删除（OWNER_REC_DELETE，CMD = 0x23）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x23
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x23
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.10 主人录音信息（OWNER_REC_INFO_GET，CMD = 0x24）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x24
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x24
byte3 : seq
byte4 : 0x03
byte5 : 0x00
byte6 : status
byte7 : exist
byte8 : durationSec
```

#### 4.11 安抚记录读取（PLAY_RECORD_GET，CMD = 0x33）

用途：读取设备缓存安抚记录（最多 16 条），格式与历史项目一致。

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x33           // cmdId = PLAY_RECORD_GET
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : maxCount       // 1~16
```

示例：

```
01 01 33 01 01 00 10
```

**响应帧（设备 -> APP，多包 RSP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x33           // cmdId = PLAY_RECORD_GET
byte3 : seq
byte4 : 0x0C           // payloadLen = 12
byte5 : 0x00
byte6 : status
byte7 : count          // 本次查询总条数（<=maxCount）
byte8 : index          // 当前条记录索引（0-based）
byte9  : start_sec_0   // u32 LE
byte10 : start_sec_1
byte11 : start_sec_2
byte12 : start_sec_3
byte13 : end_sec_0     // u32 LE
byte14 : end_sec_1
byte15 : end_sec_2
byte16 : end_sec_3
byte17 : tzQ15         // s8（当前实现固定 0）
```

约定：

- `end_sec=0xFFFFFFFF` 表示该条记录正在运行（尚未结束）

#### 4.12 安抚模式设置（CALM_MODE_SET，CMD = 0x30）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x30
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : mode           // 0=自动, 1=人工
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x30
byte3 : seq
byte4 : 0x02
byte5 : 0x00
byte6 : status
byte7 : modeApplied
```

#### 4.13 安抚模式查询（CALM_MODE_GET，CMD = 0x31）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x31
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x31
byte3 : seq
byte4 : 0x05
byte5 : 0x00
byte6 : status
byte7 : mode
byte8 : usOrder0
byte9 : usOrder1
byte10: usOrder2
```

#### 4.14 安抚策略设置（CALM_STRATEGY_SET，CMD = 0x37）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x37
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
byte6 : mode
byte7 : enabledMask
byte8 : measureOrderCount
byte9.. : measureOrder...
...   : usOrderCount
...   : usOrder...
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x37
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.15 安抚策略查询（CALM_STRATEGY_GET，CMD = 0x38）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x38
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x38
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
byte6 : status
byte7 : mode
byte8 : enabledMask
byte9 : measureOrderCount
byte10.. : measureOrder...
... : usOrderCount
... : usOrder...
```

#### 4.16 设置时间（TIME_SET，CMD = 0x32）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x32
byte3 : seq
byte4 : 0x05
byte5 : 0x00
byte6 : epochSec_0
byte7 : epochSec_1
byte8 : epochSec_2
byte9 : epochSec_3
byte10: tzQ15
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x32
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

#### 4.17 UID 查询（UID_GET，CMD = 0x34）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x34
byte3 : seq
byte4 : 0x00
byte5 : 0x00
```

**响应帧（设备 -> APP，共 2 包）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x34
byte3 : seq
byte4 : 0x0A
byte5 : 0x00
byte6 : status
byte7 : part          // 0 或 1
byte8..byte15 : uid8  // 8字节
```

#### 4.18 恢复出厂（FACTORY_RESET，CMD = 0x50）

**请求帧（APP -> 设备）**

```
byte0 : 0x01
byte1 : 0x01
byte2 : 0x50
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : reason        // 当前建议 0x01
```

**响应帧（设备 -> APP）**

```
byte0 : 0x01
byte1 : 0x02
byte2 : 0x50
byte3 : seq
byte4 : 0x01
byte5 : 0x00
byte6 : status
```

---

### 5. 事件（设备 -> APP）

#### 5.1 主人录音状态事件（OWNER_REC_STATUS，EVENT cmd = 0x84）

```
byte0 : 0x01
byte1 : 0x03           // msgType = EVENT
byte2 : 0x84
byte3 : seq
byte4 : 0x03
byte5 : 0x00
byte6 : eventType      // 0x01=10秒自动停录
byte7 : status
byte8 : durationSec
```

---

### 6. APP 处理建议

1. 连接后先开启 `Ctrl TX` Notify。  
2. 请求维护 `seq`，响应按 `cmdId+seq` 匹配。  
3. `PLAY_RECORD_GET(0x33)` 按 `count/index` 汇总完整列表。  
4. 若 `status != 0x00`，按错误码提示并允许重试。  