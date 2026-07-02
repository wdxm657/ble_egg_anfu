/********************************************************************************************************
 * @file    app_ctrl.h
 *
 * @brief   Application control protocol header (generic business logic over custom BLE service)
 *
 *******************************************************************************************************/
#ifndef APP_CTRL_H_
#define APP_CTRL_H_

#include "tl_common.h"
#include "app_config.h"
#include "printf.h"
#include <string.h>

/**
 * @brief   Control protocol basic definitions
 */

// Protocol version
#define CTRL_PROTO_VERSION 0x01

// Message types
enum
{
    CTRL_MSG_TYPE_CMD   = 0x01,
    CTRL_MSG_TYPE_RSP   = 0x02,
    CTRL_MSG_TYPE_EVENT = 0x03,
};

// Command IDs (can be extended freely)
enum
{
    CTRL_CMD_POWER_CTRL = 0x12,  // power on/off for auto play
    CTRL_CMD_STATUS_GET = 0x13,  // get device status (power/boundary/height)
    CTRL_CMD_VOLUME_SET = 0x14,
    CTRL_CMD_VOLUME_GET = 0x15,

    CTRL_CMD_OWNER_REC_START     = 0x20,
    CTRL_CMD_OWNER_REC_STOP      = 0x21,
    CTRL_CMD_OWNER_REC_PLAY      = 0x22,
    CTRL_CMD_OWNER_REC_DELETE    = 0x23,
    CTRL_CMD_OWNER_REC_INFO_GET  = 0x24,
    CTRL_CMD_OWNER_REC_PLAY_STOP = 0x25,
    CTRL_CMD_OWNER_REC_SAVE      = 0x26,  // 保存录音: 将 tmp 目录下的音频移到播放目录
    CTRL_CMD_CALM_MUSIC_PLAY      = 0x27,  // start calming music playback on SOC
    CTRL_CMD_CALM_MUSIC_PLAY_STOP = 0x28,  // stop calming music playback on SOC

    CTRL_CMD_CALM_MODE_SET     = 0x30,
    CTRL_CMD_CALM_MODE_GET     = 0x31,
    CTRL_CMD_CALM_RECORD_GET   = 0x33,
    CTRL_CMD_CALM_RECORD_DELETE = 0x3A,
    CTRL_CMD_CALM_STRATEGY_SET = 0x37,
    CTRL_CMD_CALM_STRATEGY_GET = 0x38,

    CTRL_CMD_TIME_SET = 0x32,  // set device time (YYYY-MM-DD HH:MM:SS)
    CTRL_CMD_UID_GET  = 0x34,  // get flash UID (16 bytes, split into 2 responses)

    CTRL_CMD_WORK_STATE_CHANGED     = 0x80,  // SOC work state event (device -> APP)
    CTRL_CMD_SOC_SESSION_START      = 0x81,  // SOC 安抚会话开始事件
    CTRL_CMD_SOC_MEASURE_EXEC       = 0x82,  // SOC 措施执行事件
    CTRL_CMD_SOC_SESSION_RESULT     = 0x83,  // SOC 会话结果事件
    CTRL_CMD_CALM_RECORD_NOTIFY     = 0x84,  // SOC 通知 APP 有安抚记录可用 event [recordCount(1)]
    CTRL_CMD_SOC_LOG_DATA           = 0x85,  // SOC 日志数据事件
    CTRL_CMD_SOC_ERROR              = 0x86,  // SOC 错误通知事件

    CTRL_CMD_TEXT_CHUNK = 0x40,  // long text transfer in chunks

    // Factory reset: clear owner voice, comfort config, comfort records

    CTRL_CMD_ULTRA_SET_25K   = 0x60,  // 设置超声波 25KHz
    CTRL_CMD_ULTRA_SET_30K   = 0x61,  // 设置超声波 30KHz
    CTRL_CMD_ULTRA_SET_DUAL  = 0x62,  // 设置超声波 25K+30K 混合
    CTRL_CMD_ULTRA_TRANS_ON  = 0x63,  // 开启超声波变压器
    CTRL_CMD_ULTRA_TRANS_OFF = 0x64,  // 关闭超声波变压器
    CTRL_CMD_ULTRA_EMIT_ON   = 0x65,  // 开始发射超声波
    CTRL_CMD_ULTRA_EMIT_OFF  = 0x66,  // 停止发射超声波
    CTRL_CMD_ULTRA_POWER     = 0x67,  // 超声波模组供电 (payload[0]=0关 1开)

    CTRL_CMD_FACTORY_RESET = 0x50,
};

// Error codes for response payload[0]
enum
{
    CTRL_STATUS_OK = 0x00,
    CTRL_STATUS_LEN_ERROR,
    CTRL_STATUS_UNSUPPORTED_CMD,
    CTRL_STATUS_PARAM_ERROR,
    CTRL_STATUS_INTERNAL_ERROR,
    CTRL_STATUS_BUSY,
    CTRL_STATUS_SOC_TIMEOUT,
    CTRL_STATUS_SOC_ERROR
};

// ATT value max length for RX/TX.
// 受 MCU RAM 限制，此处仅支持单帧 20 字节（默认 MTU=23 时 ATT 有效负载为 20）。
#define CTRL_RX_MAX_LEN          20
#define CTRL_TX_MAX_LEN          20
// 每个 TEXT_CHUNK 分片内可携带的纯文本字节数（注意头+分片字段总长必须 ≤ 20）
#define CTRL_TEXT_CHUNK_DATA_MAX 10

// Global RX/TX buffers used by ATT layer & control layer
extern u8 g_ctrlRxBuf[CTRL_RX_MAX_LEN];
extern u8 g_ctrlTxBuf[CTRL_TX_MAX_LEN];

/* BLE 连接状态，由 app.c 的 task_connect/task_terminate 更新 */
extern u8 g_ble_connected;

/**
 * @brief   Check if SOC is online via heartbeat
 * @return  1=online, 0=offline
 */
u8 app_ctrl_is_soc_online(void);

/**
 * @brief   Initialize control module
 */
void app_ctrl_init(void);

/**
 * @brief   Time cache tick task (1s update).
 */
void app_ctrl_time_task(void);

/**
 * @brief   Poll motor direction control limit and notify APP when reached
 */
void app_ctrl_motor_dir_task(void);

/**
 * @brief   Called by ATT write callback when CTRL_RX characteristic receives data
 *
 * @param[in] data - pointer to received data (already copied to g_ctrlRxBuf)
 * @param[in] len  - data length
 */
void app_ctrl_onRx(u8 *data, u16 len);

/**
 * @brief   Send one control frame to APP via CTRL_TX characteristic
 *
 * @param[in] msgType    - CTRL_MSG_TYPE_xxx
 * @param[in] cmdId      - CTRL_CMD_xxx
 * @param[in] seq        - sequence number
 * @param[in] payload    - payload buffer
 * @param[in] payloadLen - payload length
 *
 * @return 0: success, other: fail
 */
int app_ctrl_send(u8 msgType, u8 cmdId, u8 seq, u8 *payload, u16 payloadLen);

/**
 * @brief   Send arbitrary bytes/text to PC visualizer via Ctrl TX notify (EVENT: CTRL_CMD_TEXT_CHUNK, 0x40).
 *          Best-effort: if not connected, the call returns without sending.
 */
void app_ctrl_text_send_bytes(const u8 *data, u16 len);

/**
 * @brief   Notify APP that new calm record is available (via TEXT_CHUNK).
 *          Called on BLE connection or when SOC completes a new record.
 *          APP decides whether to call CALM_RECORD_GET(0x33) to read the record.
 */
void app_ctrl_notify_new_record(void);

static inline void app_ctrl_text_send_str(const char *s)
{
    if (!s)
    {
        return;
    }
    app_ctrl_text_send_bytes((const u8 *)s, (u16)strlen(s));
}

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#if DEBUG_MODE
/**
 * @brief   Use this macro at LOG sites to send the formatted string to PC over BLE.
 *          It does NOT hook printf, and does NOT use FIFO buffering.
 *
 * @note    Buffer is limited (local stack). Long lines will be truncated.
 *          Payload is chunked internally to fit 20-byte ATT values.
 */
#define BLE_LOG_D(fmt, ...)                                                                      \
    do                                                                                           \
    {                                                                                            \
        char _ble_log_buf[128];                                                                  \
        tl_sprintf(_ble_log_buf, "[%s:%d]: " fmt "\r\n", __FILENAME__, __LINE__, ##__VA_ARGS__); \
        app_ctrl_text_send_bytes((const u8 *)_ble_log_buf, (u16)strlen(_ble_log_buf));           \
    } while (0)
#else
#define BLE_LOG_D(fmt, ...) ((void)0)
#endif

#endif /* APP_CTRL_H_ */
