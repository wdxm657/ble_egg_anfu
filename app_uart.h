#ifndef APP_UART_H
#define APP_UART_H

#include "tl_common.h"

#define UART_PROTO_VER 0x01
#define UART_MSG_REQ   0x01
#define UART_MSG_RSP   0x02
#define UART_MSG_EVT   0x03
#define APP_UART_MAX_PAYLOAD 64

typedef void (*app_uart_rsp_cb_t)(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData);
typedef void (*app_uart_evt_cb_t)(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData);

enum{
    UART_SOC_POWER_CTRL         = 0x10,
    UART_SOC_STATUS_GET         = 0x11,
    UART_SOC_VOLUME_SET         = 0x12,
    UART_SOC_OWNER_REC_START    = 0x20,
    UART_SOC_OWNER_REC_STOP     = 0x21,
    UART_SOC_OWNER_REC_PLAY     = 0x22,
    UART_SOC_OWNER_REC_DELETE   = 0x23,
    UART_SOC_OWNER_REC_INFO_GET = 0x24,
    UART_SOC_CALM_MODE_SET      = 0x30,
    UART_SOC_CALM_STRATEGY_SET  = 0x31,
    UART_SOC_TIME_SET           = 0x32,
    UART_SOC_LOG_PULL           = 0x40,
    UART_SOC_FACTORY_RESET      = 0x50,
    UART_SOC_BT_LINK_NOTIFY     = 0x60,
};

/**
 * @brief     Initialize UART for SOC communication.
 * @param[in] none.
 * @return    none.
 */
void app_uart_init(void);

/**
 * @brief     UART NDMA interrupt process.
 * @param[in] none.
 * @return    none.
 */
void app_uart_ndma_irq_proc(void);

/**
 * @brief     UART task in main loop (send test string + report RX logs).
 * @param[in] none.
 * @return    none.
 */
void app_uart_task(void);

/**
 * @brief     Send one REQ frame to SOC.
 * @param[in] cmdId      command id.
 * @param[in] payload    payload pointer, nullable when payloadLen=0.
 * @param[in] payloadLen payload length.
 * @param[out] out_seq   optional seq output.
 * @return    0 success, negative for failure.
 */
int app_uart_send_cmd(u8 cmdId, const u8 *payload, u16 payloadLen, u8 *out_seq);

/**
 * @brief     Send REQ frame and register one-shot response callback by cmdId/seq.
 * @param[in] cmdId      command id.
 * @param[in] payload    payload pointer, nullable when payloadLen=0.
 * @param[in] payloadLen payload length, max APP_UART_MAX_PAYLOAD.
 * @param[in] rsp_cb     one-shot callback for matched RSP frame.
 * @param[in] userData   callback context.
 * @param[out] out_seq   optional seq output.
 * @return    0 success, negative for failure.
 */
int app_uart_send_cmd_with_cb(
    u8 cmdId,
    const u8 *payload,
    u16 payloadLen,
    app_uart_rsp_cb_t rsp_cb,
    void *userData,
    u8 *out_seq);

/**
 * @brief     Register persistent EVT callback for one cmdId.
 * @note      Setting evt_cb to NULL clears the registration.
 */
void app_uart_register_evt_handler(u8 cmdId, app_uart_evt_cb_t evt_cb, void *userData);

#endif /* APP_UART_H */
