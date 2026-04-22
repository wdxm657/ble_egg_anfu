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
#define CTRL_PROTO_VERSION               0x01

// Message types
enum{
    CTRL_MSG_TYPE_CMD   = 0x01,
    CTRL_MSG_TYPE_RSP   = 0x02,
    CTRL_MSG_TYPE_EVENT = 0x03,
};

// Command IDs (can be extended freely)
enum{
    CTRL_CMD_POWER_CTRL = 0x12,   // power on/off for auto play
    CTRL_CMD_STATUS_GET = 0x13,   // get device status (power/boundary/height)

    CTRL_CMD_TIME_SET        = 0x32,   // set device time (YYYY-MM-DD HH:MM:SS)
    CTRL_CMD_UID_GET         = 0x34,   // get flash UID (16 bytes, split into 2 responses)

	CTRL_CMD_TEXT_CHUNK = 0x40,   // long text transfer in chunks
};

// Error codes for response payload[0]
enum{
    CTRL_STATUS_OK              = 0x00,
    CTRL_STATUS_LEN_ERROR       = 0x01,
    CTRL_STATUS_UNSUPPORTED_CMD = 0x02,
    CTRL_STATUS_PARAM_ERROR     = 0x03,
    CTRL_STATUS_INTERNAL_ERROR  = 0x04,
};

// ATT value max length for RX/TX.
// 受 MCU RAM 限制，此处仅支持单帧 20 字节（默认 MTU=23 时 ATT 有效负载为 20）。
#define CTRL_RX_MAX_LEN                 20
#define CTRL_TX_MAX_LEN                 20
// 每个 TEXT_CHUNK 分片内可携带的纯文本字节数（注意头+分片字段总长必须 ≤ 20）
#define CTRL_TEXT_CHUNK_DATA_MAX        10

// Global RX/TX buffers used by ATT layer & control layer
extern u8 g_ctrlRxBuf[CTRL_RX_MAX_LEN];
extern u8 g_ctrlTxBuf[CTRL_TX_MAX_LEN];


/**
 * @brief   Initialize control module
 */
void app_ctrl_init(void);

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

static inline void app_ctrl_text_send_str(const char *s)
{
    if (!s)
    {
        return;
    }
    app_ctrl_text_send_bytes((const u8 *)s, (u16)strlen(s));
}

#if DEBUG_MODE
/**
 * @brief   Use this macro at LOG sites to send the formatted string to PC over BLE.
 *          It does NOT hook printf, and does NOT use FIFO buffering.
 *
 * @note    Buffer is limited (local stack). Long lines will be truncated.
 *          Payload is chunked internally to fit 20-byte ATT values.
 */
#define BLE_LOG_D(fmt, ...)                                                                 \
    do                                                                                      \
    {                                                                                       \
        char _ble_log_buf[96];                                                              \
        tl_sprintf(_ble_log_buf, fmt "\r\n", ##__VA_ARGS__);                                \
        app_ctrl_text_send_bytes((const u8 *)_ble_log_buf, (u16)strlen(_ble_log_buf));      \
    } while (0)
#else
#define BLE_LOG_D(fmt, ...) ((void)0)
#endif

#endif /* APP_CTRL_H_ */

