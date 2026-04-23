# 蛋安抚器 BLE + MCU<->SOC 串口接口设计（V1）

## 1. 目标与范围

- 目标：先完成协议与接口层设计，支持 APP、BLE MCU、SOC 并行开发。
- 范围：
  - APP <-> BLE MCU：BLE GATT 协议（控制、查询、上报、日志拉取）。
  - BLE MCU <-> SOC：UART 私有二进制协议（执行控制、状态回传、事件上报）。
- 不含：SOC 业务算法具体实现（后续补）。

---

## 2. 业务功能映射（需覆盖）

1. 配对 / 解绑（解绑触发恢复出厂）。
2. 设备开关机（上电自动开机，APP 可关机）。
3. 状态同步（监测/识别/执行/休息、蓝牙连接、录音是否存在、音量等）。
4. 主人录音管理（开始录制、结束录制、试听播放、删除、时长校验 3~10 秒）。
5. 安抚设置：
   - 模式：自动调整 / 人工干预
   - 措施选择（至少 1 项）
   - 超声组合与顺序配置（手动模式可排序）
6. 自动安抚流程执行与过程上报（检测 -> 判断 -> 多步骤执行 -> 成功/失败）。
7. 安抚日志上报与拉取（近16次）。
8. 解除配对后的工厂重置（清录音、清历史偏好、断连）。

---

## 3. BLE 接口设计

## 3.1 GATT 结构

- Service UUID：`00 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- Char-1 `Ctrl RX`（APP -> 设备，Write/WriteNoRsp）  
  UUID：`01 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`
- Char-2 `Ctrl TX`（设备 -> APP，Notify）  
  UUID：`02 A0 0D 0C 0B 0A 09 08 07 06 05 04 03 02 01 00`

> 备注：延续现有 `app_ctrl` 单通道控制模型，后续若日志吞吐不足，再新增日志专用 Notify 特征。

## 3.2 BLE 统一帧格式

```
Byte0   ver        协议版本，固定 0x01
Byte1   msgType    0x01=CMD, 0x02=RSP, 0x03=EVENT
Byte2   cmdId
Byte3   seq        请求序号，响应原样回显
Byte4   lenL
Byte5   lenH
Byte6.. payload
```

- 字节序：小端。
- 响应时限建议：控制类 <= 500ms；涉及 SOC 执行可先 ACK，再异步 EVENT。

## 3.3 通用错误码（RSP payload[0]）

- `0x00` OK
- `0x01` LEN_ERROR
- `0x02` UNSUPPORTED_CMD
- `0x03` PARAM_ERROR
- `0x04` BUSY
- `0x05` STATE_CONFLICT（当前状态不允许）
- `0x06` NO_OWNER_VOICE（无主人录音）
- `0x07` STORAGE_ERROR
- `0x08` SOC_TIMEOUT
- `0x09` INTERNAL_ERROR

## 3.4 状态枚举

- 工作状态 `workState`
  - `0x00` OFF（关机）
  - `0x01` MONITORING（监测）
  - `0x02` IDENTIFYING（识别中）
  - `0x03` ACTING（执行中）
  - `0x04` RESTING（休息中）
- 安抚模式 `calmMode`
  - `0x00` AUTO_ADJUST
  - `0x01` MANUAL_OVERRIDE

## 3.5 CMD 接口清单（APP -> 设备）

### 3.5.0 响应字段通用约定

- `status`：响应状态码（RSP payload[0]），APP 必须先判断此字段：
  - `0x00`：成功，后续字段有效。
  - `0x01`：参数或长度错误（按当前接口语义归并为参数错误），后续字段仅作参考。
  - `0x03`：业务参数错误（如录音时长不足 3 秒），APP 应提示用户重试。
  - `0x06`：无主人录音（当前映射 `NO_OWNER_VOICE`），APP 应引导先录音。
  - `0x08`：SOC 超时，APP 可做重试或提示设备忙。
  - `0x09`：内部错误，APP 提示稍后重试。
- `seq`：请求序号回显，APP 需用它匹配当前请求与响应。
- 仅当 `status=0x00` 时，建议用后续字段刷新页面状态缓存。

3) `0x13 STATUS_GET`  
- 功能：获取设备当前运行状态快照（用于设备首页轮询展示）。  
- 请求：空  
- 响应：  
  `[status, powerState, workState, btLinked, ownerVoiceExist, volume, calmMode, enabledMask, usMask]`
  - `btLinked`：兼容字段，当前固定为 `1`（APP 能拉状态即表示已连接）
- 响应字段解读与使用：
  - `powerState`：0=关机，1=开机；用于首页开关状态展示。
  - `workState`：0=OFF，1=监测，2=识别中，3=执行中，4=休息中。
  - `btLinked`：兼容字段，固定 1；APP 可忽略，不用于业务判断。
  - `ownerVoiceExist`：0=无录音，1=有录音；用于“主人声音”入口是否可用。
  - `volume`：当前音量（0~30）；用于音量滑条初始化。
  - `calmMode`：0=自动调整，1=人工干预；用于模式单选初始化。
  - `enabledMask`：安抚措施使能位；用于措施勾选状态恢复。
  - `usMask`：超声组合使能位；用于超声子选项勾选状态恢复。
- 示例请求帧（hex）：`01 01 13 01 00 00`
- 示例响应帧（hex）：`01 02 13 01 09 00 00 01 01 01 01 1E 00 05 07`

4) `0x12 POWER_SET`  
- 功能：设置设备开关机状态（电源状态由 BLE MCU 本地维护）。  
- 请求：`[onOff]`（0=关机，1=开机）  
- 响应：`[status, onOff]`
- 响应字段解读与使用：
  - `onOff`：设备最终应用的开关值（0/1）。
- 示例请求帧（hex）：`01 01 12 01 01 00 01`
- 示例响应帧（hex）：`01 02 12 01 02 00 00 01`

5) `0x14 VOLUME_SET`  
- 功能：设置扬声器音量（SOC 执行 `rk_mpi_amix_test` 后返回实际应用值）。  
- 请求：`[volume]`（范围 `0~30`）  
- 响应：`[status, volumeApplied]`
- 响应字段解读与使用：
  - `volumeApplied`：SOC 实际生效音量（0~30）。
  - APP 需使用 `volumeApplied` 更新滑条，避免显示与设备不一致。
- 示例请求帧（hex）：`01 01 14 01 01 00 1E`
- 示例响应帧（hex）：`01 02 14 01 02 00 00 1E`
- 失败响应帧示例（参数超范围）：`01 02 14 01 02 00 03 00`

6) `0x15 VOLUME_GET`  
- 功能：读取当前缓存音量（由最近一次状态同步/设置结果维护）。  
- 请求：空  
- 响应：`[status, volume]`
- 响应字段解读与使用：
  - `volume`：当前音量（0~30）。
  - APP 进入详情页时可先调用该接口初始化音量控件，也可以使用STATUS_GET获取全局信息。
- 示例请求帧（hex）：`01 01 15 01 00 00`
- 示例响应帧（hex）：`01 02 15 01 02 00 00 1E`

7) `0x20 OWNER_REC_START`  
- 功能：开始主人录音，音频落盘到 `/userdata/owner.pcm`。  
- 请求：空  
- 响应：`[status]`
- 行为：开始录制到 `/userdata/owner.pcm`，并暂停音频 AI 识别。
- 响应字段解读与使用：
  - 成功后 APP 应进入“录音中”态并启动计时（最长 10 秒）。
  - 若失败，保持“未录音”状态并提示用户重试。
- 示例请求帧（hex）：`01 01 20 01 00 00`
- 示例响应帧（hex）：`01 02 20 01 01 00 00`

8) `0x21 OWNER_REC_STOP`  
- 功能：结束主人录音，返回本次录音时长。  
- 请求：空  
- 响应：`[status, durationSec]`  
- 业务错误：`durationSec < 3` 返回 `PARAM_ERROR`，并清除本次录音临时文件。
- 录制时长上限：`10s`（超过自动截止）；结束后恢复音频 AI 识别。
- 响应字段解读与使用：
  - `durationSec`：本次录音秒数（0~10）。
  - 当 `status=0x00` 且 `durationSec>=3`：APP 标记“已录音”并展示时长。
  - 当 `status=0x03`：APP 提示“录音至少 3 秒”，并保持未录音状态。
- 示例请求帧（hex）：`01 01 21 01 00 00`
- 示例响应帧（hex）：`01 02 21 01 02 00 00 06`（录音 6 秒）
- 失败响应帧示例（录音不足 3 秒）：`01 02 21 01 02 00 03 02`

9) `0x22 OWNER_REC_PLAY`  
- 功能：播放主人录音文件 `/userdata/owner.pcm`。  
- 请求：空  
- 响应：`[status]`（无录音则 `NO_OWNER_VOICE`）
- 响应字段解读与使用：
  - `status=0x00`：播放命令下发成功，APP 可展示“播放中”状态。
  - `status=0x06`：无录音可播，APP 应提示先录音。
- 示例请求帧（hex）：`01 01 22 01 00 00`
- 示例响应帧（hex）：`01 02 22 01 01 00 00`
- 失败响应帧示例（无录音文件）：`01 02 22 01 02 00 06 00`

10) `0x23 OWNER_REC_DELETE`  
- 功能：删除主人录音文件。  
- 请求：空  
- 响应：`[status]`
- 响应字段解读与使用：
  - `status=0x00`：删除成功，APP 应清空录音时长并禁用“主人声音”措施。
  - `status=0x06`：设备侧无文件，APP 也应视作“已删除”状态。
- 示例请求帧（hex）：`01 01 23 01 00 00`
- 示例响应帧（hex）：`01 02 23 01 01 00 00`
- 失败响应帧示例（无录音文件）：`01 02 23 01 02 00 06 00`

11) `0x24 OWNER_REC_INFO_GET`  
- 功能：查询主人录音是否存在及时长（秒）。  
- 请求：空  
- 响应：`[status, exist, durationSec]`
- 响应字段解读与使用：
  - `exist`：0=无录音，1=有录音。
  - `durationSec`：录音时长（秒，最大 10）。
  - APP 进入录音页面时应先调用该接口恢复展示态。
- 示例请求帧（hex）：`01 01 24 01 00 00`
- 示例响应帧（hex）：`01 02 24 01 03 00 00 01 08`

12) `0x30 CALM_MODE_SET`  
- 请求：`[mode]`  
- 响应：`[status, modeApplied]`

13) `0x31 CALM_MODE_GET`  
- 请求：空  
- 响应：`[status, mode, effectiveUsOrder(3B)]`  
- 自动模式下返回设备当前“有效超声顺序”。

14) `0x33 CALM_STRATEGY_SET`  
- 请求：  
  `[mode, enabledMask, measureOrderCount, measureOrder..., usOrderCount, usOrder...]`
  - `enabledMask` bit0 音乐，bit1 主人录音，bit2 超声
  - `measureOrder` 元素：1=音乐，2=主人录音，3=超声
  - `usOrder` 元素：1=25kHz@100dB，2=30kHz@80dB，3=25&30kHz@100dB
- 响应：`[status]`
- 规则校验：至少一个措施；手动模式按请求顺序执行；自动模式超声顺序可动态调整。

15) `0x35 CALM_STRATEGY_GET`  
- 请求：空  
- 响应：  
  `[status, mode, enabledMask, measureOrderCount, measureOrder..., usOrderCount, usOrder...]`

17) `0x32 TIME_SET`  
- 请求：`[epochSec(u32), tzQ15(s8)]`  
- 响应：`[status]`

18) `0x34 UID_GET`  

## 3.6 EVENT 接口清单（设备 -> APP）

1) `0x80 WORK_STATE_CHANGED`  
- `payload=[workState, reason]`

2) `0x81 CALM_SESSION_STARTED`  
- `payload=[sessionId(4B), barkTs(4B)]`

3) `0x82 CALM_MEASURE_EXECUTED`  
- `payload=[sessionId(4B), stepIdx, measureType, subType, ts(4B)]`
  - `measureType`: 1音乐 2主人录音 3超声
  - `subType`：超声时对应 1/2/3，其它填 0

4) `0x83 CALM_SESSION_RESULT`  
- `payload=[sessionId(4B), result, endTs(4B), successMeasureType, successSubType]`
  - `result`：0失败，1成功

5) `0x84 OWNER_REC_STATUS`  
- `payload=[eventType, status, durationSec]`
  - `eventType`：
    - `0x01`：录音达到 10s 上限后，SOC 自动停止录音（无需 APP/MCU 再下发 STOP）
  - `status`（BLE 侧状态码语义）：
    - `0x00`：成功结束
    - `0x03`：参数/时长不满足（例如录音 <3s 被判无效）
    - `0x09`：内部错误
  - `durationSec`：最终录音时长（0~10）
  - APP 使用建议：
    - 收到该事件后应立即退出“录制中”UI 状态；
    - 当 `status=0x00` 时，用 `durationSec` 更新录音信息；
    - 当 `status!=0x00` 时，提示录音失败并可引导重录。

6) `0x85 FACTORY_RESET_DONE`  
- `payload=[reason]`（例如 `0x01` 来自解绑）

7) `0x86 ERROR_REPORT`  
- `payload=[errClass, errCode]`

---

## 4. MCU <-> SOC 串口协议设计

## 4.1 串口参数与链路约束

- 物理链路：`PC1(TX) / PC0(RX)` 对接 SOC UART。
- 串口参数：`115200 8N1`。
- 帧结构采用固定头 + 长度 + CRC16，支持 ACK/异步事件。

## 4.2 UART 帧格式

```
Byte0   SOF0      0x55
Byte1   SOF1      0xAA
Byte2   ver       0x01
Byte3   msgType   0x01=REQ 0x02=RSP 0x03=EVT
Byte4   cmdId
Byte5   seq
Byte6   lenL
Byte7   lenH
Byte8.. payload
ByteN   crcL      CRC16-IBM(从ver到payload末)
ByteN+1 crcH
```

## 4.3 UART 命令清单（MCU -> SOC）

> 以下是“BLE MCU 需要发送给 SOC 进行工作的控制命令”完整集。

1) `0x10 SOC_POWER_CTRL`  
- `payload=[onOff]`

2) `0x11 SOC_STATUS_GET`  
- `payload=[]`，请求 SOC 回传运行状态快照。

3) `0x12 SOC_VOLUME_SET`  
- `payload=[volume]`

4) `0x20 SOC_OWNER_REC_START`  
- `payload=[]`

5) `0x21 SOC_OWNER_REC_STOP`  
- `payload=[]`

6) `0x22 SOC_OWNER_REC_PLAY`  
- `payload=[]`

7) `0x23 SOC_OWNER_REC_DELETE`  
- `payload=[]`

8) `0x24 SOC_OWNER_REC_INFO_GET`  
- `payload=[]`

9) `0x30 SOC_CALM_MODE_SET`  
- `payload=[mode]`

10) `0x31 SOC_CALM_STRATEGY_SET`  
- 与 BLE `CALM_STRATEGY_SET` 结构一致，MCU 透传并可做基础参数校验。

11) `0x32 SOC_TIME_SET`  
- `payload=[epochSec(u32), tzQ15(s8)]`
- 处理约定：
  - SOC 使用 `clock_settime/settimeofday` 落系统时间（`epochSec`）。
  - SOC 使用 `setenv("TZ", ...)+tzset()` 落系统时区（`tzQ15`，15 分钟单位）。

12) `0x40 SOC_LOG_PULL`  
- `payload=[days, pageIdx, pageSize]`

13) `0x50 SOC_FACTORY_RESET`  
- `payload=[reason]`（1=解绑触发）

14) `0x60 SOC_BT_LINK_STATE_NOTIFY`  
- `payload=[linked]`（给 SOC 感知 App 在线状态，可选）

## 4.4 UART SOC -> MCU 事件清单（必须支持）

1) `0x80 SOC_WORK_STATE_EVT`  
- `payload=[workState, reason]`

2) `0x81 SOC_BARK_DETECTED_EVT`  
- `payload=[sessionId(4B), barkTs(4B)]`

3) `0x82 SOC_MEASURE_EXEC_EVT`  
- `payload=[sessionId(4B), stepIdx, measureType, subType, ts(4B)]`

4) `0x83 SOC_SESSION_RESULT_EVT`  
- `payload=[sessionId(4B), result, endTs(4B), successMeasureType, successSubType]`

5) `0x84 SOC_OWNER_REC_EVT`  
- `payload=[eventType, status, durationSec]`
  - `eventType=0x01`：录音到达 `10s` 上限后 SOC 自动停录（无需 MCU 再发 STOP）。
  - `status`：按 BLE 侧状态码语义映射（`0x00=OK,0x03=PARAM_ERROR,0x09=INTERNAL_ERROR`）。
  - `durationSec`：最终录音秒数（0~10）。
  - MCU 收到后应立即退出本地“录制中”状态，并向 APP 透出录音结束事件。

6) `0x85 SOC_LOG_DATA_EVT`  
- `payload=[chunkIdx, chunkTotal, data...]`

7) `0x86 SOC_ERROR_EVT`  
- `payload=[errClass, errCode]`

## 4.5 UART 响应约定（SOC -> MCU）

- RSP payload 最少 1 字节：`status`
  - `0x00` OK
  - `0x01` PARAM_ERROR
  - `0x02` BUSY
  - `0x03` STATE_CONFLICT
  - `0x04` TIMEOUT
  - `0x05` NOT_FOUND
  - `0x06` INTERNAL_ERROR

---

## 5. 关键业务时序（协议视角）

1) 设备上电 -> MCU 默认开机状态 -> APP 连接后读状态。  
2) APP 发录音开始/结束 -> MCU 转发 SOC；SOC 回录音结果；MCU 同步 BLE 事件。  
3) 检测到吠叫 -> SOC 事件上报执行过程 -> MCU 透出 BLE EVENT + 形成日志缓存。  
4) APP 拉日志 -> MCU 优先本地缓存，不足时请求 SOC 分页补齐。  
5) APP 解绑 -> MCU 触发 `SOC_FACTORY_RESET` -> 清状态 -> BLE 通知并断开连接。  

---

## 6. 实施优先级建议

1. 先实现最小闭环：`STATUS_GET / POWER_SET / WORK_STATE_EVT`。  
2. 再打通录音闭环：`REC_START/STOP/PLAY/DELETE` + 异常时长。  
3. 再实现安抚策略与执行过程事件。  
4. 最后补日志分页拉取与自动模式“最近成功优先”细节。  

---

## 7. 版本管理建议

- 协议版本：`v1.0.0`（本文档）。
- 变更原则：
  - 新增字段优先“尾部追加”；
  - `cmdId` 不复用；
  - 向后兼容需保留旧字段语义；
  - 破坏性改动升级 `ver`。

