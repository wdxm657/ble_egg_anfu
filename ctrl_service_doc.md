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

**阅读约定**：下文「请求帧 / 响应帧」中，`byte0`～`byte5` 为固定 6 字节帧头；`byte6` 起为负载。`seq` 由 APP 自选 `0x00`～`0xFF`，响应中原样回显。多字节整数均为**小端（LE）**。

---

### 2. 通用帧头（每个接口共用，共 6 字节）

| 字节索引 | 固定/变量 | 含义 |
|---------|-----------|------|
| byte0 | 固定 `0x01` | 协议版本 |
| byte1 | `0x01` / `0x02` / `0x03` | 消息类型：`0x01`=CMD（APP→设备），`0x02`=RSP（设备→APP），`0x03`=EVENT（设备→APP） |
| byte2 | 命令 ID | 与具体接口对应（见各节） |
| byte3 | `seq` | 序号；RSP/EVENT 与触发该结果的 CMD 使用同一 `seq`（事件除外见各节说明） |
| byte4 | `payloadLen & 0xFF` | 负载长度低字节 |
| byte5 | `(payloadLen >> 8) & 0xFF` | 负载长度高字节；当前实现恒为 `0x00` |

`payloadLen` = 从 `byte6` 开始的字节数，且满足 `6 + payloadLen <= 20`。

---

### 3. 错误码（响应或事件中 payload 的首字节 `status`）

| 取值 | 名称 | 含义（APP 侧建议） |
|------|------|-------------------|
| `0x00` | OK | 成功 |
| `0x01` | LEN_ERROR | 长度/格式不符 |
| `0x02` | UNSUPPORTED_CMD | 不支持的命令 |
| `0x03` | PARAM_ERROR | 参数非法 |
| `0x04` | INTERNAL_ERROR | 内部或下游处理失败 |
| `0x05` | BUSY | 忙（如上一笔异步未完成） |
| `0x06` | STATE_CONFLICT | 状态冲突（当前不允许该操作） |
| `0x07` | NO_OWNER_VOICE | 无主人录音文件 |
| `0x08` | STORAGE_ERROR | 存储相关错误 |
| `0x09` | SOC_TIMEOUT | 等 SOC 串口响应超时 |

**短错误响应（常见 2 字节负载）**：`byte6=status`，`byte7=errDetail`（多数为 `0x00`；部分接口在失败时 `byte7` 为 SOC 原始错误码，见各接口说明）。

---

### 4. 已实现命令（逐字节说明）

---

#### 4.1 电源开关（POWER_CTRL，CMD = `0x12`）

**请求帧（APP → 设备），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x12` | POWER_CTRL |
| byte3 | `seq` | 序号 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `onOff` | `0x00`=关机，`非0` 视为开机（固件规范化为 `0x01`） |

**响应帧（设备 → APP），总长 8 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x12` | POWER_CTRL |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x02` | 负载长度 = 2 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3；成功为 `0x00` |
| byte7 | `onOff` | 回显 MCU 应用后的开关：`0x00` 关，`0x01` 开 |

**参数错误时**：同上总长 8 字节，`byte6=0x03`（PARAM_ERROR），`byte7=0x00`。

---

#### 4.2 状态查询（STATUS_GET，CMD = `0x13`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x13` | STATUS_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 15 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x13` | STATUS_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x09` | 负载长度 = 9 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `powerState` | 电源：`0x00` 关机，`0x01` 开机（**MCU 本地维护**，非 SOC 直传） |
| byte8 | `workState` | 工作状态：`0x00` OFF，`0x01` 监测，`0x02` 识别，`0x03` 执行，`0x04` 休息 |
| byte9 | `btLinked` | 兼容字段：当前连接下**固定为 `0x01`** |
| byte10 | `ownerVoiceExist` | 主人录音是否存在：`0x00` 无，`0x01` 有 |
| byte11 | `volume` | 音量 `0`～`30` |
| byte12 | `calmMode` | 安抚模式：`0x00` 自动调整，`0x01` 人工干预 |
| byte13 | `enabledMask` | 安抚措施使能：bit0=音乐，bit1=主人录音，bit2=超声 |
| byte14 | `usMask` | 超声子项使能：bit0=25kHz，bit1=30kHz，bit2=25kHz+30kHz |

---

#### 4.3 音量设置（VOLUME_SET，CMD = `0x14`）

**请求帧（APP → 设备），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x14` | VOLUME_SET |
| byte3 | `seq` | 序号 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `volume` | 目标音量 `0`～`30` |

**响应帧（设备 → APP），总长 8 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x14` | VOLUME_SET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x02` | 负载长度 = 2 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `volumeApplied` | SOC 应用后的音量 `0`～`30` |

**失败（SOC 回包异常）**：总长 8 字节，`byte6=0x04`（INTERNAL_ERROR），`byte7=0x00`。

---

#### 4.4 音量查询（VOLUME_GET，CMD = `0x15`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x15` | VOLUME_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 8 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x15` | VOLUME_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x02` | 负载长度 = 2 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `volume` | 当前音量 `0`～`30` |

---

#### 4.5 主人录音开始（OWNER_REC_START，CMD = `0x20`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x20` | OWNER_REC_START |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧成功（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x20` | OWNER_REC_START |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` 表示 SOC 已接受开始录制 |

**响应帧失败（SOC 非 OK）**，总长 8 字节：`byte6=0x04`（INTERNAL_ERROR），`byte7=socStatus`（SOC 响应首字节原始值）。

---

#### 4.6 主人录音结束（OWNER_REC_STOP，CMD = `0x21`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x21` | OWNER_REC_STOP |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧成功（设备 → APP），总长 8 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x21` | OWNER_REC_STOP |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x02` | 负载长度 = 2 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` |
| byte7 | `durationSec` | 有效录音时长（秒） |

**响应帧（录音过短等，SOC `status=0x01`）**，总长 8 字节：同上 `byte4=0x02`，`byte6=0x03`（PARAM_ERROR），`byte7=durationSec`（SOC 带回的秒数，常为 0）。

**响应帧（其它失败）**，总长 8 字节：`byte6=0x04`（INTERNAL_ERROR），`byte7=0x00`。

---

#### 4.7 主人录音播放（OWNER_REC_PLAY，CMD = `0x22`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x22` | OWNER_REC_PLAY |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x22` | OWNER_REC_PLAY |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` 表示已接受并开始异步播放 |

**响应帧（无主人录音，SOC `NOT_FOUND`）**，总长 8 字节：`byte4=0x02`，`byte6=0x07`（NO_OWNER_VOICE），`byte7=0x00`。

**响应帧（其它失败）**，总长 8 字节：`byte6=0x04`（INTERNAL_ERROR），`byte7=0x00`。

---

#### 4.8 主人录音停止播放（OWNER_REC_PLAY_STOP，CMD = `0x25`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x25` | OWNER_REC_PLAY_STOP |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x25` | OWNER_REC_PLAY_STOP |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |

---

#### 4.9 主人录音删除（OWNER_REC_DELETE，CMD = `0x23`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x23` | OWNER_REC_DELETE |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧成功（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x23` | OWNER_REC_DELETE |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` |

**无文件**：总长 8 字节，`byte6=0x07`（NO_OWNER_VOICE），`byte7=0x00`。

---

#### 4.10 主人录音信息（OWNER_REC_INFO_GET，CMD = `0x24`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x24` | OWNER_REC_INFO_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 9 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x24` | OWNER_REC_INFO_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x03` | 负载长度 = 3 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `exist` | `0x00` 无文件，`0x01` 有 |
| byte8 | `durationSec` | 时长（秒），与 SOC 规则一致（含上限） |

---

#### 4.11 安抚记录读取（CALM_RECORD_GET，CMD = `0x33`；与旧项目「PLAY_RECORD_GET」同号）

用途：按条返回安抚会话记录；**一条记录对应一帧 RSP**（可能连续多帧 NOTIFY）。

**请求帧（APP → 设备），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x33` | CALM_RECORD_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `maxCount` | 希望最多返回条数，`1`～`16` |

**单条记录响应帧（设备 → APP），总长 18 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x33` | CALM_RECORD_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x0C` | 负载长度 = 12 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 本条为 `0x00` |
| byte7 | `count` | 本查询实际返回的总条数（≤ maxCount） |
| byte8 | `index` | 当前记录索引，`0`～`count-1` |
| byte9 | `startSec_b0` | 开始时间 Unix 秒，uint32 LE 字节 0 |
| byte10 | `startSec_b1` | 字节 1 |
| byte11 | `startSec_b2` | 字节 2 |
| byte12 | `startSec_b3` | 字节 3 |
| byte13 | `endSec_b0` | 结束时间 Unix 秒，uint32 LE；`0xFF,0xFF,0xFF,0xFF` 表示进行中 |
| byte14 | `endSec_b1` |  |
| byte15 | `endSec_b2` |  |
| byte16 | `endSec_b3` |  |
| byte17 | `tzQ15` | 时区扩展，当前固件固定填 `0x00`（占位） |

**错误短包示例（总长 8 字节）**：`byte4=0x02`，`byte6=status`，`byte7=0x00`（如 BUSY、SOC_TIMEOUT）。

**错误 3 字节包（总长 9 字节）**：首条解析失败时 `byte4=0x03`，`byte6~byte8` 依实现（多为 `status` + 保留 `0`）。

---

#### 4.12 安抚模式设置（CALM_MODE_SET，CMD = `0x30`）

**请求帧（APP → 设备），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x30` | CALM_MODE_SET |
| byte3 | `seq` | 序号 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `mode` | `0x00` 自动调整（允许动态调序），`0x01` 人工干预（固定策略顺序） |

**响应帧成功（设备 → APP），总长 8 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x30` | CALM_MODE_SET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x02` | 负载长度 = 2 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` |
| byte7 | `modeApplied` | 与请求一致或 SOC 确认后的值 |

---

#### 4.13 安抚模式查询（CALM_MODE_GET，CMD = `0x31`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x31` | CALM_MODE_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），总长 11 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x31` | CALM_MODE_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x05` | 负载长度 = 5 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `mode` | 当前安抚模式，同 §4.12 |
| byte8 | `usOrder0` | 超声策略顺序第 1 步：`1`=25k，`2`=30k，`3`=25k+30k |
| byte9 | `usOrder1` | 第 2 步；未使用可填 `0` |
| byte10 | `usOrder2` | 第 3 步；未使用可填 `0` |

（`usOrder*` 来自 MCU 缓存的超声顺序，与策略里超声段一致。）

---

#### 4.14 安抚策略设置（CALM_STRATEGY_SET，CMD = `0x37`）

负载为变长：`payloadLen = 3 + M + 1 + U`，其中 `M = measureOrderCount`（1～3），`U = usOrderCount`（1～3）。整帧须满足 `6 + payloadLen <= 20`。

**帧头（byte0～byte5）**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x37` | CALM_STRATEGY_SET |
| byte3 | `seq` | 序号 |
| byte4 | `payloadLen & 0xFF` | 例如 `M=2,U=3` 时 `payloadLen=9`，此处为 `0x09` |
| byte5 | `0x00` | 长度高字节 |

**负载首段（从 byte6 起）**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte6 | `mode` | 与 §4.12 相同；仅允许 `0` 或 `1` |
| byte7 | `enabledMask` | bit0=音乐，bit1=主人录音，bit2=超声；**不可为 0** |
| byte8 | `measureOrderCount` | `M`，执行顺序项个数，1～3 |
| byte9 ～ `byte(8+M)` | `measureOrder[i]` | 每项：`1`=音乐，`2`=主人录音，`3`=超声 |

**负载末段（超声顺序）**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| `byte(9+M)` | `usOrderCount` | `U`，1～3 |
| 随后 `U` 字节 | `usOrder[j]` | 每项：`1`=25kHz，`2`=30kHz，`3`=25kHz+30kHz |

**响应帧成功（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x37` | CALM_STRATEGY_SET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` |

**响应帧失败（SOC 非 OK）**，总长 8 字节：`byte6=0x04`（INTERNAL_ERROR），`byte7=socErr`（SOC 状态字节）。

---

#### 4.15 安抚策略查询（CALM_STRATEGY_GET，CMD = `0x38`）

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x38` | CALM_STRATEGY_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧（设备 → APP），变长**  
令 `M = measureOrderCount`，`U = usOrderCount`，则 `payloadLen = 4 + M + U`（最大 `4+3+3=10`），总长 `6 + payloadLen`，不超过 20。

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x38` | CALM_STRATEGY_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `payloadLen & 0xFF` | 见上式 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `mode` | 安抚模式 |
| byte8 | `enabledMask` | 同 §4.14 |
| byte9 | `measureOrderCount` | `M` |
| byte10 ～ `byte(9+M)` | `measureOrder[i]` | 同 §4.14 |
| `byte(10+M)` | `usOrderCount` | `U` |
| 随后 `U` 字节 | `usOrder[j]` | 同 §4.14 |

---

#### 4.16 设置时间（TIME_SET，CMD = `0x32`）

**请求帧（APP → 设备），总长 11 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x32` | TIME_SET |
| byte3 | `seq` | 序号 |
| byte4 | `0x05` | 负载长度 = 5 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `epochSec_b0` | Unix 时间戳（秒），uint32 LE 字节 0 |
| byte7 | `epochSec_b1` | 字节 1 |
| byte8 | `epochSec_b2` | 字节 2 |
| byte9 | `epochSec_b3` | 字节 3 |
| byte10 | `tzQ15` | 有符号时区参数（固件侧与 SOC 约定，通常为 15 分钟粒度偏移）；具体换算见 `BLE_UART接口设计_V1.md` |

**响应帧（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x32` | TIME_SET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` 表示 MCU 已缓存并下发 SOC（SOC 设时结果以联调为准） |

---

#### 4.17 UID 查询（UID_GET，CMD = `0x34`）

Flash UID 共 16 字节，分 **2 帧 RSP** 回传，每帧 8 字节负载。

**请求帧（APP → 设备），总长 6 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x34` | UID_GET |
| byte3 | `seq` | 序号 |
| byte4 | `0x00` | 负载长度 = 0 |
| byte5 | `0x00` | 长度高字节 |

**响应帧第 1 包（设备 → APP），总长 16 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x34` | UID_GET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x0A` | 负载长度 = 10 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `part` | 固定 `0x00`，表示 UID 第 1 段 |
| byte8～byte15 | `uid[0..7]` | UID 字节 0～7 |

**响应帧第 2 包（设备 → APP），总长 16 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x34` | UID_GET |
| byte3 | `seq` | 与第 1 包相同 |
| byte4 | `0x0A` | 负载长度 = 10 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | 见 §3 |
| byte7 | `part` | 固定 `0x01`，表示 UID 第 2 段 |
| byte8～byte15 | `uid[8..15]` | UID 字节 8～15 |

---

#### 4.18 恢复出厂（FACTORY_RESET，CMD = `0x50`）

**请求帧（APP → 设备），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x01` | CMD |
| byte2 | `0x50` | FACTORY_RESET |
| byte3 | `seq` | 序号 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `reason` | 原因码；当前建议 `0x01`（如解绑触发），其它值保留 |

**响应帧（设备 → APP），总长 7 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x02` | RSP |
| byte2 | `0x50` | FACTORY_RESET |
| byte3 | `seq` | 与请求一致 |
| byte4 | `0x01` | 负载长度 = 1 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `status` | `0x00` 表示已执行清录音、清策略与清记录等出厂数据流程 |

---

### 5. 事件（设备 → APP，逐字节）

#### 5.1 主人录音状态事件（OWNER_REC_STATUS，EVENT cmd = `0x84`）

**事件帧，总长 9 字节**

| 字节 | 值/变量 | 注释 |
|------|---------|------|
| byte0 | `0x01` | 协议版本 |
| byte1 | `0x03` | EVENT |
| byte2 | `0x84` | OWNER_REC_STATUS |
| byte3 | `seq` | 事件序号（由设备分配，可与 CMD 无关） |
| byte4 | `0x03` | 负载长度 = 3 |
| byte5 | `0x00` | 长度高字节 |
| byte6 | `eventType` | `0x01`：录音达到 10s 上限由 SOC 自动停止 |
| byte7 | `status` | 与 §3 语义一致，表示停录结果 |
| byte8 | `durationSec` | 停录时有效时长（0～10） |

---

### 6. APP 处理建议

1. 连接后先开启 `Ctrl TX` Notify。  
2. 请求维护 `seq`，响应按 `cmdId + seq` 匹配。  
3. `CALM_RECORD_GET(0x33)` 按多条 RSP 的 `count/index` 汇总；注意帧间间隔由设备侧处理，APP 应逐条接收。  
4. 若 `status != 0x00`，结合 §3 与对应接口的 `errDetail`（若有）提示用户或重试。  
