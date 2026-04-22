## 通用控制 Service 协议说明（给 APP 开发）

### 1. GATT 结构概览

- **Service：Custom Control Service**
- UUID（16 字节）：`00 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`()
- **Characteristic 1：Ctrl RX（APP → 设备）**
- UUID：`01 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- 属性：Read | Write | Write Without Response
- User Description：`Ctrl RX`
- 用途：APP 向设备发送所有指令 / 配置信息。
- **Characteristic 2：Ctrl TX（设备 → APP）**
- UUID：`02 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- 属性：Read | Notify
- User Description：`Ctrl TX`
- 需先向其 CCC 写 `0x0001` 以开启 Notify。
- 用途：设备向 APP 返回响应、状态、事件。

ATT Value 有效负载最大长度由 MTU 决定：`maxLen = MTU - 3`。当前固件出于 MCU RAM 考量，仅实现默认 **MTU=23** 的场景，即：**单帧最大 20 字节**。若 MTU 被协商为更大，仍建议单帧控制在 20 字节以内。

---

### 2. 通用帧格式（Ctrl RX / Ctrl TX 共用）

所有命令、响应、事件统一使用如下帧格式：

```
byte0 : version      协议版本，目前固定 0x01
byte1 : msgType      消息类型
byte2 : cmdId        命令 ID
byte3 : seq          序号（请求-响应对应）
byte4 : payloadLen L 负载长度低字节
byte5 : payloadLen H 负载长度高字节
byte6.. : payload    具体负载，长度 = payloadLen
```

- **msgType**
- `0x01`：命令（APP → 设备）
- `0x02`：响应（设备 → APP）
- **payloadLen**
- 小端：`len = byte4 + (byte5 << 8)`；
- **单帧长度上限**（当前实现）：`6 + len ≤ 20` 字节。超出 20 字节的“业务数据”需要在应用层拆分为多帧。

APP 写 Ctrl RX、设备通过 Ctrl TX Notify 返回的内容均从 `byte0` 开始。

---

### 3. 错误码（响应 payload[0]）

设备响应帧的 `payload[0]` 用作状态码：

- `0x00`：OK（成功）
- `0x01`：LEN_ERROR（长度错误）
- `0x02`：UNSUPPORTED_CMD（不支持的命令）
- `0x03`：PARAM_ERROR（参数错误）
- `0x04`：INTERNAL_ERROR（内部错误）

建议统一响应负载结构：

```
payload[0] = status      // 上述错误码
payload[1] = errDetail   // 目前为 0，预留
... 其余字段依具体命令而定
```

---

### 4. 已实现命令列表及完整帧结构

以下每个命令/响应都给出**完整帧字段含义**（从 `byte0` 开始），APP 可直接按此封包 / 解包。

#### 4.1 LED 控制（CMD = 0x10）

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x10           // cmdId = LED_CTRL
byte3 : seq            // 0~255, 由 APP 决定
byte4 : 0x02           // payloadLen L = 2
byte5 : 0x00           // payloadLen H = 0
byte6 : ledId          // 0:全部, 1:蓝, 2:绿, 3:白, 4:红
byte7 : state          // 0:关, 1:开

01 01 10 01 02 00 00 01
```

**响应帧（设备 → APP）**

```
byte0 : 0x01           // version
byte1 : 0x02           // msgType = RSP
byte2 : 0x10           // cmdId = LED_CTRL
byte3 : seq            // 与请求一致
byte4 : 0x02           // payloadLen L = 2
byte5 : 0x00           // payloadLen H = 0
byte6 : status         // 0x00 成功，其它为错误码
byte7 : 0x00           // errDetail，当前为 0
```

#### 4.2 二轴电机控制（MOTOR_CTRL，CMD = 0x20）

该命令用于控制 Pan/Tilt 两轴步进电机，支持：查询当前角度坐标信息。

- 轴定义：`0 = Pan`，`1 = Tilt`

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x20           // cmdId = MOTOR_CTRL
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
01 01 20 01 06 00
byte6 : op
byte7.. : parameters by op
```

`op` 定义：

1. `op = 0x02`：查询当前角度坐标信息

**示例**：

```
01 01 20 01 01 00 02
```

**响应帧（设备 → APP）**

- `op = 0x02` 查询响应：

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x20           // cmdId = MOTOR_CTRL
byte3 : seq
byte4 : 0x06           // payloadLen = 6
byte5 : 0x00
byte6 : status
byte7 : 0x02           // 回显 op
byte8 : x_L            // 当前坐标 x (mm)
byte9 : x_H
byte10: y_L            // 当前坐标 y (mm)
byte11: y_H
```

#### 4.3 四方向电机控制（MOTOR_DIR_CTRL，CMD = 0x22）

用于 APP 端通过 **上/下/左/右** 按键控制电机运动到机械限位（阈值由固件限位决定），可选速度档位。

- 方向定义：
- `0x00`：上（TILT+）
- `0x01`：下（TILT-）
- `0x02`：左（PAN-）
- `0x03`：右（PAN+）
- 速度档位：
- `0`：默认速度（与原 MOTOR_CTRL 的默认一致）
- `1`：慢
- `2`：中
- `3`：快

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x22           // cmdId = MOTOR_DIR_CTRL
byte3 : seq
byte4 : 0x03           // payloadLen = 3
byte5 : 0x00
byte6 : op             // 0x00=stop, 0x01=move
byte7 : direction      // 上下左右
byte8 : speedLevel     // 0/1/2/3
```

**示例**：

```
（停止）
01 01 22 01 03 00 00 01 02
（下转，中速）
01 01 22 01 03 00 01 01 02
（右转，中速）
01 01 22 01 03 00 01 03 02
01 01 20 01 01 00 02
```

**响应帧（设备 → APP）**（立即响应）

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x22           // cmdId = MOTOR_DIR_CTRL
byte3 : seq
byte4 : 0x03           // payloadLen = 3
byte5 : 0x00
byte6 : status
byte6 : direction
byte7 : op             // 回显 0x00/0x01
```

**事件帧（设备 → APP）**

1. 到达机械限位时：

```
byte0 : 0x01
byte1 : 0x03           // msgType = EVENT
byte2 : 0x22           // cmdId = MOTOR_DIR_CTRL
byte3 : seq            // 事件序号
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : 0x01           // evtId=到达限位
byte7 : direction
```

2. 边界点设置模式下触发“越界保护”时：

```
byte0 : 0x01
byte1 : 0x03           // msgType = EVENT
byte2 : 0x22           // cmdId = MOTOR_DIR_CTRL
byte3 : seq            // 事件序号
byte4 : 0x04           // payloadLen = 4
byte5 : 0x00
byte6 : 0x02           // evtId=边界越界保护触发
byte7 : direction      // 0x00=上,0x01=下,0x02=左,0x03=右
byte8 : pointIndex     // 当前正在设置的点
byte9 : limitPointIndex// 约束参考点
```

3. **仅边界标定模式**下，俯仰“上”达到当前安装高度对应的地面投射最大距离（固件按 6 m 水平距离推算的俯仰上限）时：

```
byte0 : 0x01
byte1 : 0x03           // msgType = EVENT
byte2 : 0x22           // cmdId = MOTOR_DIR_CTRL
byte3 : seq            // 事件序号
byte4 : 0x04           // payloadLen = 4
byte5 : 0x00
byte6 : 0x03           // evtId=安装高度俯仰上限（6 m 投射几何上限）
byte7 : direction      // 固定为 0x00（上）
byte8 : tilt_deg10_L   // 当前俯仰角 deg×10，小端 s16
byte9 : tilt_deg10_H
```

说明：在边界点设置模式中，设备会在 PAN/TILT 两方向做约束，即将越界时自动停止电机并上报 evtId=0x02。另：evtId=0x03 仅在边界标定流程中、且已设置安装高度时可能上报（俯仰“上”超过按 6 m 地面投射推算的上限时）；设备停止俯仰并锁角，普通四向控制（非标定）不触发 0x03。

约束规则：

- 右移（0x03）：LU(0) 受 RU(1) 约束；LD(3) 受 RD(2) 约束。
- 左移（0x02）：RU(1) 受 LU(0) 约束；RD(2) 受 LD(3) 约束。
- 上移（0x00）：LD(3) 受 LU(0) 约束；RD(2) 受 RU(1) 约束。
- 下移（0x01）：LU(0) 受 LD(3) 约束；RU(1) 受 RD(2) 约束。

#### 4.4 设置电机原点（MOTOR_SET_ZERO，CMD = 0x21）

用于把“当前角度”设为逻辑 0°，便于现场校准。

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x21           // cmdId = MOTOR_SET_ZERO
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : axis           // 0:Pan, 1:Tilt, 0xFF:两轴同时置零
01 01 21 01 01 00 ff
```

说明：当电机先运动到某个角度后，发送该命令即可把该位置作为新原点。之后查询角度和目标角度控制都基于该新原点。

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x21           // cmdId = MOTOR_SET_ZERO
byte3 : seq
byte4 : 0x04           // payloadLen = 4
byte5 : 0x00
byte6 : status
byte7 : axis           // 回显请求 axis
byte8 : 0x00           // 预留
byte9 : 0x00           // 预留
```

#### 4.5 电源开关（POWER_CTRL，CMD = 0x12）

用途：APP 控制自动逗宠开关。`1` 开启，`0` 关闭。

**示例**：

```
（开启）
01 01 12 01 01 00 01
（关闭）
01 01 12 01 01 00 00
```

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x12           // cmdId = POWER_CTRL
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : on             // 0x00=关, 0x01=开
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x12           // cmdId = POWER_CTRL
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : on             // 回显 0x00/0x01
```

#### 4.6 设备状态查询（STATUS_GET，CMD = 0x13）

用途：获取设备开关机、逗宠区域是否设置、安装高度是否设置。

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x13           // cmdId = STATUS_GET
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x13           // cmdId = STATUS_GET
byte3 : seq
byte4 : 0x04           // payloadLen = 4
byte5 : 0x00
byte6 : status
byte7 : power_on       // 0x00=关, 0x01=开
byte8 : play_zone_set  // 0x00=未设置, 0x01=已设置
byte9 : height_lo      // 高度 低位
byte10 : height_hi     // 高度 高位
```

#### 4.7 设置设备时间（TIME_SET，CMD = 0x32）

用途：APP 发送 Unix 时间戳（秒）和时区，设备侧更新当前时间。

**示例**：

```
01 01 32 01 05 00 80 D3 27 66 00
```

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x32           // cmdId = TIME_SET
byte3 : seq
byte4 : 0x05           // payloadLen = 5
byte5 : 0x00
byte6 : epochSec_L0    // Unix 时间戳（秒）低字节
byte7 : epochSec_L1
byte8 : epochSec_L2
byte9 : epochSec_L3    // Unix 时间戳（秒）高字节
byte10: tz
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x32           // cmdId = TIME_SET
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : 0x00
```

#### 4.8 逗宠记录读取（PLAY_RECORD_GET，CMD = 0x33）

用途：APP 读取设备端缓存的自动逗宠记录（最多 16 条）。每条记录包含开始/结束时间的 Unix 时间戳（秒）。

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x33           // cmdId = PLAY_RECORD_GET
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : maxCount       // 1~16
```

**示例**：

```
01 01 33 01 01 00 10
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x33           // cmdId = PLAY_RECORD_GET
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
byte6 : status
byte7 : count
byte8 : index
byte9.. : records      // 每条 9 字节：start_sec(u32 LE) + end_sec(u32 LE) + tz
```

#### 4.9 读取 SN（UID_GET，CMD = 0x34）

用途：APP 读取设备 Flash UID（16 字节）。只发一次命令，设备连续返回 2 次响应，每次 8 字节。

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x34           // cmdId = UID_GET
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**示例**：

```
01 01 34 01 00 00
```

**响应帧（设备 → APP）**（共返回 2 次）

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x34           // cmdId = UID_GET
byte3 : seq
byte4 : 0x0A           // payloadLen = 10
byte5 : 0x00
byte6 : status         // 0x00 成功，其它为错误码
byte7 : part           // 0: UID[0..7], 1: UID[8..15]
byte8..byte15 : uid8   // 8 字节 UID 分片
```

#### 4.10 设置雷达安装高度（RADAR_SET_INSTALL_HEIGHT，CMD = 0x50）

用途：APP 侧设置雷达安装高度（mm），影响 tilt 角计算。

**请求帧（APP → 设备）**

```
byte0 : 0x01           // version
byte1 : 0x01           // msgType = CMD
byte2 : 0x50           // cmdId = RADAR_SET_INSTALL_HEIGHT
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : height_L       // s16, mm
byte7 : height_H
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x50           // cmdId = RADAR_SET_INSTALL_HEIGHT
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : 0x00
```

设备行为：调用 `app_radar_set_install_height_mm()`，内部会将高度限制在 500~10000mm。

#### 4.11 边界点设置状态机（RADAR_BOUNDARY_XXX，CMD = 0x51~0x54）

用途：APP 以“进入 → 选点 → 调整电机 → 保存 → 退出”的方式设置边界四个角点。点位顺序固定为：**左上(0) → 右上(1) → 右下(2) → 左下(3)**。

##### 4.11.1 进入设置状态（RADAR_BOUNDARY_ENTER，CMD = 0x51）

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x51           // cmdId = RADAR_BOUNDARY_ENTER
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x51           // cmdId = RADAR_BOUNDARY_ENTER
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : 0x00
```

设备行为：进入边界点设置状态并清空本轮缓存点位。

##### 4.11.2 选择需要调整的点（RADAR_BOUNDARY_SELECT_POINT，CMD = 0x52）

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x52           // cmdId = RADAR_BOUNDARY_SELECT_POINT
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : pointIndex     // 0:左上, 1:右上, 2:右下, 3:左下
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x52           // cmdId = RADAR_BOUNDARY_SELECT_POINT
byte3 : seq
byte4 : 0x03           // payloadLen = 3
byte5 : 0x00
byte6 : status
byte7 : pointIndex
byte8 : 0x00           // reserved
```

设备行为：根据上一次保存的该点坐标，先驱动电机移动到对应位置。运动完成后，APP 可开始发送电机控制指令进行微调。

##### 4.11.3 保存当前点（RADAR_BOUNDARY_SAVE_POINT，CMD = 0x53）

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x53           // cmdId = RADAR_BOUNDARY_SAVE_POINT
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : pointIndex     // 当前要保存的点：0~3
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x53           // cmdId = RADAR_BOUNDARY_SAVE_POINT
byte3 : seq
byte4 : 0x04           // payloadLen = 4
byte5 : 0x00
byte6 : status
byte7 : pointIndex
byte8 : ready          // 1: 当前已收齐 4 点；0: 未收齐
byte9 : 0x00           // reserved
```

设备行为：

1. 使用请求里 `pointIndex` 指定要保存的点（不再依赖上次 select 的 active index）。
2. 读取当前电机角度，反算坐标并保存该点。
3. 仅返回当前点保存结果与 `ready` 状态，不在此命令中执行四点校验/提交。

##### 4.11.4 提交四点边界（RADAR_BOUNDARY_COMMIT，CMD = 0x55）

用途：APP 调用该接口表示“4 个点均已就绪”，设备执行顺序与距离校验并决定是否应用边界。

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x55           // cmdId = RADAR_BOUNDARY_COMMIT
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x55           // cmdId = RADAR_BOUNDARY_COMMIT
byte3 : seq
byte4 : payloadLen L
byte5 : payloadLen H
byte6 : status
byte7 : applyOkOrReserve  // 成功时=1；失败时保留
byte8 : errDetail         // 0:无, 1:两点距离<1m, 2:顺序错误, 3:状态错误, 4:点位错误
byte9 : shortPairMask     // 当 errDetail=1 时有效，位图表示所有 <1m 的点对（最多6组）
```

`shortPairMask` 位定义（4 点共 6 组）：

- bit0: (0,1)
- bit1: (0,2)
- bit2: (0,3)
- bit3: (1,2)
- bit4: (1,3)
- bit5: (2,3)

例如：

- `shortPairMask = 0x01` 表示仅 (0,1) 小于 1m
- `shortPairMask = 0x09` 表示 (0,1) 和 (1,2) 小于 1m
- 提交成功：payloadLen = 4
- 提交失败：payloadLen = 4（含 `errDetail`、`shortPairMask`）

##### 4.11.5 退出设置状态（RADAR_BOUNDARY_EXIT，CMD = 0x54）

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x54           // cmdId = RADAR_BOUNDARY_EXIT
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x54           // cmdId = RADAR_BOUNDARY_EXIT
byte3 : seq
byte4 : 0x02           // payloadLen = 2
byte5 : 0x00
byte6 : status
byte7 : 0x00
```

设备行为：退出边界点设置状态，恢复其它业务。

##### 4.11.5 示例流程（设置左上点）

1. 进入边界点设置状态：

```
01 01 51 01 00 00
```

1. 选择左上点（index=0），设备自动移动到上次左上点：

```
01 01 52 02 01 00 00
```

1. APP 使用 `MOTOR_DIR_CTRL` 指令微调电机位置。

```
（下转，中速）
01 01 22 01 03 00 01 01 02
（右转，中速）
01 01 22 01 03 00 01 03 02
（停止）
01 01 22 01 03 00 00 01 02
（查询电机角度坐标信息）
01 01 20 01 01 00 02
```

1. 保存左上点：

```
01 01 53 03 00 00
```

1. 若需要退出设置状态：

```
01 01 54 04 00 00
```

#### 4.12 文本分片传输（TEXT_CHUNK，CMD = 0x40）

用于发送长文本，设备端最多缓存 `CTRL_TEXT_MAX_TOTAL_LEN = 100` 字节文本，每帧最多携带 `CTRL_TEXT_CHUNK_DATA_MAX = 10` 字节纯文本。

**请求帧（APP → 设备）**

```
byte0 : 0x01            // version
byte1 : 0x01            // msgType = CMD
byte2 : 0x40            // cmdId = TEXT_CHUNK
byte3 : seq
byte4 : payloadLen L    // = 4 + dataLen  (dataLen <= 10)
byte5 : payloadLen H
byte6 : transferId      // 传输ID, 0~255，用于标识一次完整文本传输
byte7 : chunkIndex      // 当前分片序号，从 0 开始递增
byte8 : chunkTotal      // 本次传输的总分片数
byte9 : dataLen         // 本分片文本字节数，<= 10
byte10..(9+dataLen) : text data bytes (不要求每片结尾有 '\0')
```

约束：

- `6 + payloadLen ≤ 20`；
- `payloadLen = 4 + dataLen`，因此 `dataLen ≤ 10`；
- 所有分片的 `transferId` 和 `chunkTotal` 在一次传输过程中必须一致。

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02            // msgType = RSP
byte2 : 0x40            // cmdId = TEXT_CHUNK
byte3 : seq
byte4 : 0x03            // payloadLen = 3
byte5 : 0x00
byte6 : status          // 0x00 成功，其他错误码
byte7 : transferId      // 回显
byte8 : chunkIndex      // 回显
```

设备端行为说明：

- 收到 `chunkIndex == 0` 的首片时，会重置内部文本缓冲区，并记录本次的 `transferId` 和 `chunkTotal`；
- 每收到一片：
- 若 `4 + dataLen > payloadLen` 或 `dataLen > 10`，返回 `LEN_ERROR`；
- 若累计长度超过 100 字节，返回 `LEN_ERROR` 并丢弃本次传输；
- 否则将本片数据追加到内部缓冲区，并返回 `status = OK`；
- 当累计接收分片数达到 `chunkTotal` 时，认为文本传输完成：
- 会在本地补一个结尾 `'\0'` 方便调试日志；
- 当前实现通过日志输出完整文本：`[CTRL][TEXT] id=.. len=.. text="..."`；
- 然后清空缓冲区，等待下一次传输。

APP 侧发送完整长文本的推荐流程：

1. 假设要发送的字符串为 UTF-8 字节流 `text[]`，长度为 `N`；
2. 选择一个 `transferId`（如本地计数器取模 256）；
3. 将 `N` 按 `dataLen ≤ 10` 切分为若干块，得到 `chunkTotal`；
4. 对于每个 `chunkIndex`（0..chunkTotal-1）：

- 
- 构造请求帧，填入对应片段的数据；
- 写入 Ctrl RX；
- 等待 Ctrl TX 返回的响应，确认该片 `status == 0x00`；

1. 当所有分片均成功，应答 `status=0x00` 后，本次文本在设备端即可视为“已完整接收并处理”。

#### 4.13 复位

**请求帧（APP → 设备）**

```
byte0 : 0x01
byte1 : 0x01           // msgType = CMD
byte2 : 0x56           // cmdId = CTRL_CMD_RADAR_RESET_FLASH_CONFIG
byte3 : seq
byte4 : 0x00           // payloadLen = 0
byte5 : 0x00
```

**响应帧（设备 → APP）**

```
byte0 : 0x01
byte1 : 0x02           // msgType = RSP
byte2 : 0x56           // cmdId = CTRL_CMD_RADAR_RESET_FLASH_CONFIG
byte3 : seq
byte4 : 0x01           // payloadLen = 1
byte5 : 0x00
byte6 : status
```

设备行为：清除FLASH中的高度和坐标信息

---

### 5. 长数据与 APP 侧处理建议

1. **单帧最大长度**

- 
- 当前实现按默认 MTU=23 设计：`6 + payloadLen ≤ 20`；
- 如果 APP 写入或设备试图发送超过 20 字节的帧，设备侧会返回长度错误（不会静默截断）。

1. **需要发送超过 20 字节的业务数据时**

- 
- 必须在应用层做**分片协议设计**，例如：
- 自定义一个新的命令（例如 `DATA_CHUNK`，cmdId 由双方约定），payload 中增加：
- `chunkIndex`（当前分片序号）、`chunkTotal`（总分片数）、`offset`、`chunkLen` 等字段；
- 每一帧的总长度仍然遵守 `6 + payloadLen ≤ 20`；
- 设备端在对应 handler 中按 `chunkIndex/offset` 进行重组或流式处理。
- 本协议文件只定义了**单帧格式**，具体的分片/重组规则由业务双方根据需求单独约定并在固件中实现对应命令 handler。

---

### 6. APP 侧典型调用流程

1. 扫描并连接设备；
2. 发现包含上述 UUID 的 **Custom Control Service**；
3. 找到 `Ctrl TX` 特征，向其 CCC 写入 `0x0001` 以启用 Notify；
4. 每次发送命令时：

- 
- 维护一个本地 `seq`（0~255 循环）；
- 按「帧格式 + 命令定义」组装数据后，写入 `Ctrl RX`；

1. 在 `Ctrl TX` 的 Notify 回调中：

- 
- 解析 `version / msgType / cmdId / seq / payloadLen`；
- 校验 `version == 0x01`，`msgType == 0x02`；
- 根据 `cmdId + seq` 找到对应的请求，读取 `payload[0]` 判断是否成功，然后按各命令的 payload 协议解析剩余内容。

如需新增业务，只需约定新的 `cmdId` 和对应的 `payload` 格式，固件会按相同头部格式进行收发。