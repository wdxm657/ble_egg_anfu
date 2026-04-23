#include "tl_common.h"
#include "drivers.h"

#include "app_uart.h"
#include "app_ctrl.h"

_attribute_data_retention_ static u8 g_uart_soc_rx_buf[128];
_attribute_data_retention_ static u8 g_uart_soc_rx_len = 0;
_attribute_data_retention_ static u8 g_uart_seq        = 0;

#define APP_UART_PENDING_MAX 8

typedef struct
{
    u8                inUse;
    u8                cmdId;
    u8                seq;
    app_uart_rsp_cb_t cb;
    void             *userData;
} app_uart_pending_t;

typedef struct
{
    app_uart_evt_cb_t cb;
    void             *userData;
} app_uart_evt_handler_t;

_attribute_data_retention_ static app_uart_pending_t     g_uart_pending[APP_UART_PENDING_MAX];
_attribute_data_retention_ static app_uart_evt_handler_t g_uart_evt_handlers[256];

static u16 app_uart_crc16_ibm(const u8 *data, u16 len)
{
    u16 crc = 0xFFFF;
    for (u16 i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (u8 j = 0; j < 8; j++)
        {
            if (crc & 0x0001)
            {
                crc = (crc >> 1) ^ 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static int app_uart_send_frame(u8 msgType, u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen)
{
    if (payloadLen > APP_UART_MAX_PAYLOAD)
    {
        return -1;
    }

    u8  frame[8 + APP_UART_MAX_PAYLOAD + 2];
    u16 idx      = 0;
    frame[idx++] = 0x55;
    frame[idx++] = 0xAA;
    frame[idx++] = UART_PROTO_VER;
    frame[idx++] = msgType;
    frame[idx++] = cmdId;
    frame[idx++] = seq;
    frame[idx++] = U16_LO(payloadLen);
    frame[idx++] = U16_HI(payloadLen);
    if (payloadLen && payload)
    {
        memcpy(&frame[idx], payload, payloadLen);
        idx += payloadLen;
    }

    u16 crc      = app_uart_crc16_ibm(&frame[2], (u16)(6 + payloadLen));
    frame[idx++] = U16_LO(crc);
    frame[idx++] = U16_HI(crc);

    for (u16 i = 0; i < idx; i++)
    {
        uart_ndma_send_byte(frame[i]);
    }
    return 0;
}

int app_uart_send_cmd(u8 cmdId, const u8 *payload, u16 payloadLen, u8 *out_seq)
{
    u8 seq = g_uart_seq++;
    if (out_seq)
    {
        *out_seq = seq;
    }
    return app_uart_send_frame(UART_MSG_REQ, cmdId, seq, payload, payloadLen);
}

int app_uart_send_cmd_with_cb(
    u8                cmdId,
    const u8         *payload,
    u16               payloadLen,
    app_uart_rsp_cb_t rsp_cb,
    void             *userData,
    u8               *out_seq)
{
    u8 seq = g_uart_seq++;
    if (out_seq)
    {
        *out_seq = seq;
    }

    if (rsp_cb)
    {
        int slot = -1;
        for (u8 i = 0; i < APP_UART_PENDING_MAX; i++)
        {
            if (!g_uart_pending[i].inUse)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
        {
            return -2;
            BLE_LOG_D("app_uart_send_cmd_with_cb slot < 0");
        }
        g_uart_pending[slot].inUse    = 1;
        g_uart_pending[slot].cmdId    = cmdId;
        g_uart_pending[slot].seq      = seq;
        g_uart_pending[slot].cb       = rsp_cb;
        g_uart_pending[slot].userData = userData;
    }

    if (app_uart_send_frame(UART_MSG_REQ, cmdId, seq, payload, payloadLen) != 0)
    {
        if (rsp_cb)
        {
            for (u8 i = 0; i < APP_UART_PENDING_MAX; i++)
            {
                if (g_uart_pending[i].inUse && g_uart_pending[i].cmdId == cmdId && g_uart_pending[i].seq == seq)
                {
                    g_uart_pending[i].inUse = 0;
                    break;
                }
            }
        }
        BLE_LOG_D("app_uart_send_cmd_with_cb send_frame failed");
        return -1;
    }
    return 0;
}

void app_uart_register_evt_handler(u8 cmdId, app_uart_evt_cb_t evt_cb, void *userData)
{
    g_uart_evt_handlers[cmdId].cb       = evt_cb;
    g_uart_evt_handlers[cmdId].userData = userData;
}

static void app_uart_dispatch_rsp(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen)
{
    for (u8 i = 0; i < APP_UART_PENDING_MAX; i++)
    {
        if (g_uart_pending[i].inUse && g_uart_pending[i].cmdId == cmdId && g_uart_pending[i].seq == seq)
        {
            app_uart_rsp_cb_t cb       = g_uart_pending[i].cb;
            void             *userData = g_uart_pending[i].userData;
            g_uart_pending[i].inUse    = 0;  // one-shot
            if (cb)
            {
                cb(cmdId, seq, payload, payloadLen, userData);
            }
            return;
        }
    }
    BLE_LOG_D("[SOC_RSP] unmatched cmd=0x%02x seq=%d len=%d", cmdId, seq, payloadLen);
}

static void app_uart_dispatch_evt(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen)
{
    app_uart_evt_cb_t cb = g_uart_evt_handlers[cmdId].cb;
    if (cb)
    {
        cb(cmdId, seq, payload, payloadLen, g_uart_evt_handlers[cmdId].userData);
    }
    else
    {
        BLE_LOG_D("[SOC_EVT] unhandled cmd=0x%02x seq=%d len=%d", cmdId, seq, payloadLen);
    }
}

static void app_uart_try_parse_one(void)
{
    if (g_uart_soc_rx_len < 10)
    {
        return;
    }

    u16 start = 0;
    while ((start + 1) < g_uart_soc_rx_len)
    {
        if (g_uart_soc_rx_buf[start] == 0x55 && g_uart_soc_rx_buf[start + 1] == 0xAA)
        {
            break;
        }
        start++;
    }
    if ((start + 1) >= g_uart_soc_rx_len)
    {
        g_uart_soc_rx_len = 0;
        return;
    }
    if (start > 0)
    {
        memmove(g_uart_soc_rx_buf, &g_uart_soc_rx_buf[start], g_uart_soc_rx_len - start);
        g_uart_soc_rx_len = (u8)(g_uart_soc_rx_len - start);
    }

    if (g_uart_soc_rx_len < 10)
    {
        return;
    }

    u16 payloadLen = g_uart_soc_rx_buf[6] | (((u16)g_uart_soc_rx_buf[7]) << 8);
    u16 frameLen   = (u16)(8 + payloadLen + 2);
    if (frameLen > sizeof(g_uart_soc_rx_buf))
    {
        g_uart_soc_rx_len = 0;
        return;
    }
    if (g_uart_soc_rx_len < frameLen)
    {
        return;
    }

    u16 recvCrc = g_uart_soc_rx_buf[frameLen - 2] | (((u16)g_uart_soc_rx_buf[frameLen - 1]) << 8);
    u16 calcCrc = app_uart_crc16_ibm(&g_uart_soc_rx_buf[2], (u16)(6 + payloadLen));
    if (recvCrc == calcCrc)
    {
        u8        msgType = g_uart_soc_rx_buf[3];
        u8        cmdId   = g_uart_soc_rx_buf[4];
        u8        seq     = g_uart_soc_rx_buf[5];
        const u8 *payload = &g_uart_soc_rx_buf[8];
        BLE_LOG_D("[SOC_FRAME] t=0x%02x cmd=0x%02x seq=%d len=%d", msgType, cmdId, seq, payloadLen);

        if (msgType == UART_MSG_RSP)
        {
            app_uart_dispatch_rsp(cmdId, seq, payload, payloadLen);
        }
        else if (msgType == UART_MSG_EVT)
        {
            app_uart_dispatch_evt(cmdId, seq, payload, payloadLen);
        }
    }
    else
    {
        BLE_LOG_D("[SOC_FRAME] crc_err recv=0x%04x calc=0x%04x", recvCrc, calcCrc);
    }

    if (g_uart_soc_rx_len > frameLen)
    {
        memmove(g_uart_soc_rx_buf, &g_uart_soc_rx_buf[frameLen], g_uart_soc_rx_len - frameLen);
    }
    g_uart_soc_rx_len = (u8)(g_uart_soc_rx_len - frameLen);
}

void app_uart_init(void)
{
    // USART initial for SOC communication (PC1:TX, PC0:RX, 115200 8N1)
    uart_gpio_set(GPIO_PC1, GPIO_PC0);
    uart_init_baudrate(115200, CLOCK_SYS_CLOCK_HZ, PARITY_NONE, STOP_BIT_ONE);
    uart_dma_enable(0, 0);
    uart_ndma_clear_rx_index();
    uart_ndma_clear_tx_index();
    uart_ndma_irq_triglevel(1, 0);
    uart_irq_enable(1, 0);
    memset(g_uart_pending, 0, sizeof(g_uart_pending));
    memset(g_uart_evt_handlers, 0, sizeof(g_uart_evt_handlers));
}

void app_uart_ndma_irq_proc(void)
{
    if (!uart_ndmairq_get())
    {
        return;
    }

    unsigned char rx_cnt = reg_uart_buf_cnt & 0x0f;

    while (rx_cnt--)
    {
        u8 data = uart_ndma_read_byte();
        if (g_uart_soc_rx_len < sizeof(g_uart_soc_rx_buf))
        {
            g_uart_soc_rx_buf[g_uart_soc_rx_len++] = data;
        }
        else
        {
            g_uart_soc_rx_len = 0;
        }
    }
}

void app_uart_task(void)
{
    while (g_uart_soc_rx_len >= 10)
    {
        u8 before = g_uart_soc_rx_len;
        app_uart_try_parse_one();
        if (g_uart_soc_rx_len == before)
        {
            break;
        }
    }
}
