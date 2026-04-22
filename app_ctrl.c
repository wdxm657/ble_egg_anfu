/********************************************************************************************************
 * @file    app_ctrl.c
 *
 * @brief   Application control protocol implementation
 *          - Generic command channel over custom BLE service
 *          - Handles LED control, motor control, configuration, etc.
 *
 *******************************************************************************************************/

#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "app_config.h"
#include "app.h"
#include "app_att.h"
#include "app_ctrl.h"
#include "app_uart.h"

#include "app.h"

// RX/TX buffers shared with ATT layer
u8 g_ctrlRxBuf[CTRL_RX_MAX_LEN] = {0};
u8 g_ctrlTxBuf[CTRL_TX_MAX_LEN] = {0};

// simple sequence generator for events/async notifications
static u8 g_ctrlSeq = 0;

typedef struct
{
    u8 powerState;          // 电源状态: 0=关机, 1=开机(由 BLE MCU 本地维护)
    u8 workState;           // 当前工作状态: 0=OFF, 1=监测, 2=识别, 3=执行, 4=休息
    u8 btLinked;            // 兼容字段: 对当前连接会话固定返回 1（保留给旧版 APP）
    u8 ownerVoiceExist;     // 主人录音是否存在: 0=无, 1=有
    u8 ownerVoiceDuration;  // 主人录音时长(秒)
    u8 volume;              // 音量值(当前约定 0~30)
    u8 calmMode;            // 安抚模式: 0=自动调整, 1=人工干预
    u8 enabledMask;         // 安抚措施使能位: bit0=音乐 bit1=主人录音 bit2=超声
    u8 usMask;              // 超声子措施使能位: bit0=25kHz bit1=30kHz bit2=25&30kHz
    u8 measureOrderCount;   // 安抚措施执行顺序项数(最多 3)
    u8 measureOrder[3];     // 安抚措施执行顺序: 1=音乐 2=主人录音 3=超声
    u8 usOrderCount;        // 超声执行顺序项数(最多 3)
    u8 usOrder[3];          // 超声执行顺序: 1=25kHz 2=30kHz 3=25&30kHz
} app_ctrl_state_t;

static app_ctrl_state_t g_ctrlState = {
    .powerState         = 1,
    .workState          = 1,
    .btLinked           = 1,
    .ownerVoiceExist    = 0,
    .ownerVoiceDuration = 0,
    .volume             = 30,
    .calmMode           = 0,
    .enabledMask        = 0x05,
    .usMask             = 0x07,
    .measureOrderCount  = 2,
    .measureOrder       = {1, 3, 0},
    .usOrderCount       = 3,
    .usOrder            = {1, 2, 3},
};

typedef struct
{
    u8 bleSeq;
} app_ctrl_ble_req_ctx_t;

_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_status_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_info_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_calm_mode_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_volume_set;

static void app_ctrl_rsp_status_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 8)
    {
        // 约定: [status,power,work,bt,ownerRec,volume,calmMode,enabledMask,usMask]
        // 注意: powerState 由 BLE MCU 本地维护，不使用 SOC 上报值覆盖。
        if (payload[0] == 0x00)
        {
            g_ctrlState.workState       = payload[2];
            // btLinked 为兼容字段，固定由 MCU 侧返回 1，不从 SOC 回包覆盖。
            g_ctrlState.ownerVoiceExist = payload[4];
            g_ctrlState.volume          = payload[5];
            g_ctrlState.calmMode        = payload[6];
            g_ctrlState.enabledMask     = payload[7];
            if (payloadLen >= 9)
            {
                g_ctrlState.usMask = payload[8];
            }
        }
    }

    u8 rsp[9] = {
        CTRL_STATUS_OK,
        g_ctrlState.powerState,
        g_ctrlState.workState,
        g_ctrlState.btLinked,
        g_ctrlState.ownerVoiceExist,
        g_ctrlState.volume,
        g_ctrlState.calmMode,
        g_ctrlState.enabledMask,
        g_ctrlState.usMask,
    };
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_STATUS_GET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_owner_rec_info_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 3 && payload[0] == 0x00)
    {
        // 约定: [status,exist,duration]
        g_ctrlState.ownerVoiceExist    = payload[1];
        g_ctrlState.ownerVoiceDuration = payload[2];
    }

    u8 rsp[3] = {CTRL_STATUS_OK, g_ctrlState.ownerVoiceExist, g_ctrlState.ownerVoiceDuration};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_INFO_GET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_calm_mode_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 7 && payload[0] == 0x00)
    {
        // 复用 STATUS_GET 回包: [status,power,work,bt,ownerRec,volume,calmMode,enabledMask,usMask]
        g_ctrlState.calmMode = payload[6];
    }

    u8 rsp[5] = {CTRL_STATUS_OK, g_ctrlState.calmMode, g_ctrlState.usOrder[0], g_ctrlState.usOrder[1], g_ctrlState.usOrder[2]};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_GET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_volume_set_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 2 && payload[0] == 0x00)
    {
        g_ctrlState.volume = payload[1];
        u8 rsp[2]          = {CTRL_STATUS_OK, g_ctrlState.volume};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, ctx->bleSeq, rsp, sizeof(rsp));
    }
}

// ----------------------- sending -----------------------
int app_ctrl_send(u8 msgType, u8 cmdId, u8 seq, u8 *payload, u16 payloadLen)
{
    u16 headerLen = 1 + 1 + 1 + 1 + 2;  // version + msgType + cmdId + seq + payloadLen
    u16 totalLen  = headerLen + payloadLen;

    if (totalLen > CTRL_TX_MAX_LEN)
    {
        // payload too long for one ATT Value, do not send and report error to caller
        return -1;
    }

    u8 *p = g_ctrlTxBuf;
    p[0]  = CTRL_PROTO_VERSION;
    p[1]  = msgType;
    p[2]  = cmdId;
    p[3]  = seq;
    p[4]  = U16_LO(payloadLen);
    p[5]  = U16_HI(payloadLen);

    if (payloadLen && payload)
    {
        memcpy(p + headerLen, payload, payloadLen);
    }
#if (DEBUG_MODE)
    {
        // tl_printf("app_ctrl_send");
        // for (u8 i = 0; i < totalLen; i++)
        // {
        //     tl_printf("0x%01x ", p[i]);
        // }
        // tl_printf("\r\n");
    }
#endif
    if (BLS_CONN_HANDLE != 0xFFFF)
    {
        blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, CUSTOM_COUNTER_READ_DP_H, g_ctrlTxBuf, totalLen);
    }
    // memset(g_ctrlRxBuf, 0, sizeof(g_ctrlRxBuf));
    // memset(g_ctrlTxBuf, 0, sizeof(g_ctrlTxBuf));
    return 0;
}

// ----------------------- direct text/bytes to BLE (EVENT: CTRL_CMD_TEXT_CHUNK) -----------------------
// Send arbitrary bytes/text to PC visualizer via Ctrl TX notify (chunked to fit 20-byte ATT value).
static u8 g_textTxTransferId = 0;

void app_ctrl_text_send_bytes(const u8 *data, u16 len)
{
    if (!data || len == 0)
    {
        return;
    }
    if (BLS_CONN_HANDLE == 0xFFFF)
    {
        return;
    }

    u8 maxData = CTRL_TEXT_CHUNK_DATA_MAX;
    u8 total   = (u8)((len + maxData - 1) / maxData);
    if (total == 0)
    {
        total = 1;
    }

    u8 transferId = g_textTxTransferId++;

    for (u8 idx = 0; idx < total; idx++)
    {
        u16 off = (u16)idx * (u16)maxData;
        u16 rem = (len > off) ? (len - off) : 0;
        u8  dln = (rem > maxData) ? maxData : (u8)rem;

        // payload: [0]=transferId, [1]=chunkIndex, [2]=chunkTotal, [3]=dataLen, [4..]=data
        u8 pl[4 + CTRL_TEXT_CHUNK_DATA_MAX];
        pl[0] = transferId;
        pl[1] = idx;
        pl[2] = total;
        pl[3] = dln;
        if (dln)
        {
            memcpy(&pl[4], data + off, dln);
        }
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_TEXT_CHUNK, g_ctrlSeq++, pl, (u16)(4 + dln));

        // Space NOTIFYs so the stack copies g_ctrlTxBuf each time (same buffer for all sends).
        if (idx + 1 < total)
        {
            sleep_us(5000);
        }
    }
    sleep_us(10000);
}

static int app_ctrl_handle_time_set(u8 seq, u8 *payload, u16 len)
{
    if (len != 5)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u32 epoch_sec = ((u32)payload[0]) | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
    s8  tz_q15    = (s8)payload[4];

    (void)epoch_sec;
    (void)tz_q15;

    app_uart_send_cmd(UART_SOC_TIME_SET, payload, len, 0);

    u8 rsp[1] = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_power_ctrl(u8 seq, u8 *payload, u16 len)
{
    if (len < 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_POWER_CTRL, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 on = payload[0] ? 1 : 0;
    app_set_power_state(on);
    g_ctrlState.powerState = on;
    g_ctrlState.workState  = on ? 1 : 0;
    app_uart_send_cmd(UART_SOC_POWER_CTRL, &on, 1, 0);

    u8 rsp[2] = {CTRL_STATUS_OK, on};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_POWER_CTRL, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_status_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    (void)len;
    g_ctrlState.btLinked    = 1;
    g_ctx_status_get.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_STATUS_GET,
            0,
            0,
            app_ctrl_rsp_status_get_from_soc,
            &g_ctx_status_get,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_STATUS_GET, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_uid_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_UID_GET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 uid[16] = {0};
    app_get_flash_uid(uid, sizeof(uid));

    for (u8 part = 0; part < 2; part++)
    {
        u8 rsp[10] = {CTRL_STATUS_OK, part, 0, 0, 0, 0, 0, 0, 0, 0};
        u8 base    = (u8)(part * 8);
        for (u8 i = 0; i < 8; i++)
        {
            rsp[2 + i] = uid[base + i];
        }

        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_UID_GET, seq, rsp, sizeof(rsp));
    }

    return 0;
}

static int app_ctrl_handle_volume_set(u8 seq, u8 *payload, u16 len)
{
    if (len < 1 || payload[0] > 30)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, seq, rsp, sizeof(rsp));
        return -1;
    }

    g_ctx_volume_set.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_VOLUME_SET,
            payload,
            1,
            app_ctrl_rsp_volume_set_from_soc,
            &g_ctx_volume_set,
            0)
        != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_volume_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.volume};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_GET, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_owner_rec_start(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_START, seq, rsp, sizeof(rsp));
        return -1;
    }
    app_uart_send_cmd(UART_SOC_OWNER_REC_START, 0, 0, 0);
    u8 rsp[1] = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_START, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_owner_rec_stop(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, seq, rsp, sizeof(rsp));
        return -1;
    }
    app_uart_send_cmd(UART_SOC_OWNER_REC_STOP, 0, 0, 0);
    g_ctrlState.ownerVoiceDuration = 5;
    g_ctrlState.ownerVoiceExist    = (g_ctrlState.ownerVoiceDuration >= 3) ? 1 : 0;

    u8 rsp[2] = {g_ctrlState.ownerVoiceExist ? CTRL_STATUS_OK : CTRL_STATUS_PARAM_ERROR, g_ctrlState.ownerVoiceDuration};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_owner_rec_play(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!g_ctrlState.ownerVoiceExist)
    {
        u8 rsp[2] = {CTRL_STATUS_NO_OWNER_VOICE, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, seq, rsp, sizeof(rsp));
        return -1;
    }
    app_uart_send_cmd(UART_SOC_OWNER_REC_PLAY, 0, 0, 0);
    u8 rsp[1] = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_owner_rec_delete(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, seq, rsp, sizeof(rsp));
        return -1;
    }
    app_uart_send_cmd(UART_SOC_OWNER_REC_DELETE, 0, 0, 0);
    g_ctrlState.ownerVoiceExist    = 0;
    g_ctrlState.ownerVoiceDuration = 0;
    u8 rsp[1]                      = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_owner_rec_info_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_INFO_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    g_ctx_owner_rec_info_get.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_INFO_GET,
            0,
            0,
            app_ctrl_rsp_owner_rec_info_from_soc,
            &g_ctx_owner_rec_info_get,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_INFO_GET, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_calm_mode_set(u8 seq, u8 *payload, u16 len)
{
    if (len < 1 || payload[0] > 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_SET, seq, rsp, sizeof(rsp));
        return -1;
    }
    g_ctrlState.calmMode = payload[0];
    app_uart_send_cmd(UART_SOC_CALM_MODE_SET, payload, 1, 0);
    u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.calmMode};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_SET, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_calm_mode_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    g_ctx_calm_mode_get.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_STATUS_GET,
            0,
            0,
            app_ctrl_rsp_calm_mode_get_from_soc,
            &g_ctx_calm_mode_get,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_GET, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_calm_strategy_set(u8 seq, u8 *payload, u16 len)
{
    if (len < 4)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 mode        = payload[0];
    u8 enabledMask = payload[1];
    u8 measureCnt  = payload[2];
    if (mode > 1 || enabledMask == 0 || measureCnt > 3)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u16 idx = 3;
    if ((u16)(idx + measureCnt + 1) > len)
    {
        u8 rsp[2] = {CTRL_STATUS_LEN_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
        return -1;
    }
    g_ctrlState.calmMode          = mode;
    g_ctrlState.enabledMask       = enabledMask;
    g_ctrlState.measureOrderCount = measureCnt;
    memset(g_ctrlState.measureOrder, 0, sizeof(g_ctrlState.measureOrder));
    for (u8 i = 0; i < measureCnt; i++)
    {
        g_ctrlState.measureOrder[i] = payload[idx++];
    }

    u8 usCnt = payload[idx++];
    if (usCnt > 3 || (u16)(idx + usCnt) > len)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
        return -1;
    }
    g_ctrlState.usOrderCount = usCnt;
    g_ctrlState.usMask       = 0;
    memset(g_ctrlState.usOrder, 0, sizeof(g_ctrlState.usOrder));
    for (u8 i = 0; i < usCnt; i++)
    {
        u8 item                = payload[idx++];
        g_ctrlState.usOrder[i] = item;
        if (item >= 1 && item <= 3)
        {
            g_ctrlState.usMask |= (u8)(1 << (item - 1));
        }
    }

    app_uart_send_cmd(UART_SOC_CALM_STRATEGY_SET, payload, len, 0);
    u8 rsp[1] = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_calm_strategy_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 rsp[16];
    u8 n     = 0;
    rsp[n++] = CTRL_STATUS_OK;
    rsp[n++] = g_ctrlState.calmMode;
    rsp[n++] = g_ctrlState.enabledMask;
    rsp[n++] = g_ctrlState.measureOrderCount;
    for (u8 i = 0; i < g_ctrlState.measureOrderCount; i++)
    {
        rsp[n++] = g_ctrlState.measureOrder[i];
    }
    rsp[n++] = g_ctrlState.usOrderCount;
    for (u8 i = 0; i < g_ctrlState.usOrderCount; i++)
    {
        rsp[n++] = g_ctrlState.usOrder[i];
    }
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, seq, rsp, n);
    return 0;
}

// ----------------------- public APIs -----------------------
void app_ctrl_init(void)
{
    memset(g_ctrlRxBuf, 0, sizeof(g_ctrlRxBuf));
    memset(g_ctrlTxBuf, 0, sizeof(g_ctrlTxBuf));
    g_ctrlSeq            = 0;
    g_ctrlState.btLinked = 1;
}

void app_ctrl_onRx(u8 *data, u16 len)
{
    if (len < 6)
    {
        // too short, ignore
        return;
    }

    u8  version = data[0];
    u8  msgType = data[1];
    u8  cmdId   = data[2];
    u8  seq     = data[3];
    u16 payLen  = data[4] | (data[5] << 8);

    if (version != CTRL_PROTO_VERSION)
    {
        // unsupported version, reply error
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
        return;
    }

    if (msgType != CTRL_MSG_TYPE_CMD)
    {
        // only command supported from APP side
        return;
    }

    if ((u16)(6 + payLen) > len)
    {
        u8 rsp[2] = {CTRL_STATUS_LEN_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
        return;
    }

    u8 *payload = &data[6];

    switch (cmdId)
    {
    case CTRL_CMD_TIME_SET:
        BLE_LOG_D("CTRL_CMD_TIME_SET");
        app_ctrl_handle_time_set(seq, payload, payLen);
        break;
    case CTRL_CMD_STATUS_GET:
        BLE_LOG_D("CTRL_CMD_STATUS_GET");
        app_ctrl_handle_status_get(seq, payload, payLen);
        break;
    case CTRL_CMD_POWER_CTRL:
        BLE_LOG_D("CTRL_CMD_POWER_CTRL");
        app_ctrl_handle_power_ctrl(seq, payload, payLen);
        break;
    case CTRL_CMD_UID_GET:
        BLE_LOG_D("CTRL_CMD_UID_GET");
        app_ctrl_handle_uid_get(seq, payload, payLen);
        break;
    case CTRL_CMD_VOLUME_SET:
        BLE_LOG_D("CTRL_CMD_VOLUME_SET");
        app_ctrl_handle_volume_set(seq, payload, payLen);
        break;
    case CTRL_CMD_VOLUME_GET:
        BLE_LOG_D("CTRL_CMD_VOLUME_GET");
        app_ctrl_handle_volume_get(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_START:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_START");
        app_ctrl_handle_owner_rec_start(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_STOP:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_STOP");
        app_ctrl_handle_owner_rec_stop(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_PLAY:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_PLAY");
        app_ctrl_handle_owner_rec_play(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_DELETE:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_DELETE");
        app_ctrl_handle_owner_rec_delete(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_INFO_GET:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_INFO_GET");
        app_ctrl_handle_owner_rec_info_get(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_MODE_SET:
        BLE_LOG_D("CTRL_CMD_CALM_MODE_SET");
        app_ctrl_handle_calm_mode_set(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_MODE_GET:
        BLE_LOG_D("CTRL_CMD_CALM_MODE_GET");
        app_ctrl_handle_calm_mode_get(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_STRATEGY_SET:
        BLE_LOG_D("CTRL_CMD_CALM_STRATEGY_SET");
        app_ctrl_handle_calm_strategy_set(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_STRATEGY_GET:
        BLE_LOG_D("CTRL_CMD_CALM_STRATEGY_GET");
        app_ctrl_handle_calm_strategy_get(seq, payload, payLen);
        break;
    default: {
        u8 rsp[2] = {CTRL_STATUS_UNSUPPORTED_CMD, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
        break;
    }
    }
}
