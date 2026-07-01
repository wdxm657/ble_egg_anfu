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
#include "app_ultrasonic.h"

#include "app.h"

// RX/TX buffers shared with ATT layer
u8 g_ctrlRxBuf[CTRL_RX_MAX_LEN] = {0};
u8 g_ctrlTxBuf[CTRL_TX_MAX_LEN] = {0};

// simple sequence generator for events/async notifications
static u8 g_ctrlSeq = 0;

// ===================== SOC online tracking via heartbeat =====================
// SOC sends heartbeat every ~2s; if missing for SOC_HEARTBEAT_TIMEOUT_US, mark offline.
#define SOC_HEARTBEAT_TIMEOUT_US 7000000  // 7 s (tolerates ~3 lost beats)

static u8  g_soc_online              = 0;
static u32 g_soc_last_heartbeat_tick = 0;

/**
 * @brief  Public: check if SOC is currently considered online.
 * @return 1 if online, 0 if offline.
 */
u8 app_ctrl_is_soc_online(void)
{
    return g_soc_online;
}

/**
 * @brief  Internal helper: if SOC is offline, immediately reply SOC_TIMEOUT to BLE.
 * @return 1 if online (caller should continue), 0 if offline (caller should return).
 */
static int app_ctrl_check_soc_online(u8 cmdId, u8 seq)
{
    if (!g_soc_online)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
        return 0;
    }
    return 1;
}

/**
 * @brief  Callback: SOC heartbeat event received.
 */
static void app_ctrl_evt_heartbeat_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)payload;
    (void)payloadLen;
    (void)userData;

    // BLE_LOG_D("soc heart beat");
    g_soc_online              = 1;
    g_soc_last_heartbeat_tick = clock_time();
}

typedef struct
{
    u8 powerState;          // 电源状态: 0=关机, 1=开机(由 BLE MCU 本地维护)
    u8 workState;           // 当前工作状态: 0=OFF, 1=监测, 2=识别, 3=执行, 4=休息
    u8 btLinked;            // 兼容字段: 对当前连接会话固定返回 1（保留给旧版 APP）
    u8 ownerVoiceExist;     // 主人录音是否存在: 0=无, 1=有
    u8 ownerVoiceDuration;  // 主人录音时长(秒)
    u8 volume;              // 音量值(百分比 0~100)
    u8 calmMode;            // 安抚模式: 0=自动调整, 1=人工干预
    u8 enabledMask;         // 安抚措施使能位: bit0=音乐 bit1=主人录音 bit2=超声
    u8 usMask;              // 超声子措施使能位: bit0=25kHz bit1=30kHz bit2=25&30kHz
    u8 measureOrderCount;   // 安抚措施执行顺序项数(最多 3)
    u8 measureOrder[3];     // 安抚措施执行顺序: 1=音乐 2=主人录音 3=超声
    u8 usOrderCount;        // 超声执行顺序项数(最多 3)
    u8 usOrder[3];          // 超声执行顺序: 1=25kHz 2=30kHz 3=25&30kHz
} app_ctrl_state_t;

static app_ctrl_state_t g_ctrlState = {
    .powerState         = 0,
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
    u8  valid;
    u32 epochSec;
    s8  tzQ15;
    u32 lastTick;
} app_ctrl_time_cache_t;

static app_ctrl_time_cache_t g_timeCache = {0};

typedef struct
{
    u8 bleSeq;
} app_ctrl_ble_req_ctx_t;

/* 上一次推送的状态快照 (9 字节，与 STATUS_GET 响应 payload 一致) */
/* [0]=status always 0, [1]=powerState, [2]=workState, [3]=btLinked,
   [4]=ownerVoiceExist, [5]=volume, [6]=calmMode, [7]=enabledMask, [8]=usMask */
static u8 s_pushed_status[9] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/**
 * @brief 检查 g_ctrlState 是否有变化，有则推送 STATUS 事件给 APP。
 *         payload 格式与 STATUS_GET RSP 一致 (9 字节)。
 */
static void app_ctrl_state_try_push_event(void)
{
    u8 cur[9];
    cur[0] = CTRL_STATUS_OK;
    cur[1] = g_ctrlState.powerState;
    cur[2] = g_ctrlState.workState;
    cur[3] = g_ctrlState.btLinked;
    cur[4] = g_ctrlState.ownerVoiceExist;
    cur[5] = g_ctrlState.volume;
    cur[6] = g_ctrlState.calmMode;
    cur[7] = g_ctrlState.enabledMask;
    cur[8] = g_ctrlState.usMask;

    if (memcmp(s_pushed_status, cur, sizeof(cur)) == 0)
    {
        return;  /* 无变化 */
    }

    memcpy(s_pushed_status, cur, sizeof(cur));
    app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_STATUS_GET, g_ctrlSeq++, cur, sizeof(cur));
    BLE_LOG_D("[STATE_PUSH] power=%d work=%d bt=%d rec=%d vol=%d mode=%d enabled=0x%02x us=0x%02x",
              cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
}

_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_power_ctrl;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_status_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_volume_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_info_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_calm_mode_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_volume_set;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_start;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_stop;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_play;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_delete;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_play_stop;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_owner_rec_save;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_factory_reset;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_time_set;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_calm_mode_set;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_calm_strategy_set;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t g_ctx_calm_strategy_get;

typedef struct
{
    u8 bleSeq;
    u8 maxCount;
    u8 totalSend;
    u8 total;
    u8 sent;
    u8 busy;
} app_ctrl_record_req_ctx_t;

_attribute_data_retention_ static app_ctrl_record_req_ctx_t g_ctx_calm_record_get;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t    g_ctx_calm_record_delete;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t    g_ctx_calm_music_play;
_attribute_data_retention_ static app_ctrl_ble_req_ctx_t    g_ctx_calm_music_play_stop;

static void app_ctrl_time_cache_update(void)
{
    if (!g_timeCache.valid)
    {
        return;
    }

    if (clock_time() - g_timeCache.lastTick >= (1000000 * 16))
    {
        g_timeCache.epochSec++;
        g_timeCache.lastTick = clock_time();
    }
}

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
            g_ctrlState.workState = payload[2];
            // btLinked 为兼容字段，固定由 MCU 侧返回 1，不从 SOC 回包覆盖。
            g_ctrlState.ownerVoiceExist = payload[4];
            // SOC 已返回 0-100，直接使用
            g_ctrlState.volume = payload[5];
            if (g_ctrlState.volume > 100) g_ctrlState.volume = 100;
            g_ctrlState.calmMode        = payload[6];
            g_ctrlState.enabledMask     = payload[7];
            if (payloadLen >= 9)
            {
                g_ctrlState.usMask = payload[8];
            }
            BLE_LOG_D("[SOC_RSP] STATUS_GET  work=%d rec=%d vol=%d mode=%d enabled=0x%02x us=0x%02x",
                      payload[2],
                      payload[4],
                      payload[5],
                      payload[6],
                      payload[7],
                      (payloadLen >= 9) ? payload[8] : 0);
        }
        else
        {
            BLE_LOG_D("[SOC_RSP] STATUS_GET soc_status=0x%02x", payload[0]);
        }
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] STATUS_GET too short len=%d", payloadLen);
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
        BLE_LOG_D("[SOC_RSP] OWNER_REC_INFO exist=%d duration=%d", payload[1], payload[2]);
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] OWNER_REC_INFO failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
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
        BLE_LOG_D("[SOC_RSP] CALM_MODE_GET mode=%d", g_ctrlState.calmMode);
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] CALM_MODE_GET failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
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
        // SOC 已返回 0-100，直接使用
        g_ctrlState.volume = payload[1];
        if (g_ctrlState.volume > 100) g_ctrlState.volume = 100;
        BLE_LOG_D("[SOC_RSP] VOLUME_SET pc=%d", g_ctrlState.volume);
        u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.volume};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] VOLUME_SET failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
        u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, payloadLen ? payload[0] : 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, ctx->bleSeq, rsp, sizeof(rsp));
    }
}

static void app_ctrl_rsp_owner_rec_start_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] OWNER_REC_START OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_START, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] OWNER_REC_START failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
        u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, payloadLen ? payload[0] : 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_START, ctx->bleSeq, rsp, sizeof(rsp));
    }
}

static void app_ctrl_rsp_owner_rec_stop_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 2)
    {
        u8 status      = payload[0];
        u8 durationSec = payload[1];
        if (status == 0x00)
        {
            g_ctrlState.ownerVoiceDuration = durationSec;
            g_ctrlState.ownerVoiceExist    = (durationSec >= 3) ? 1 : 0;
            BLE_LOG_D("[SOC_RSP] OWNER_REC_STOP OK duration=%d", durationSec);
            u8 rsp[2] = {CTRL_STATUS_OK, durationSec};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
        if (status == 0x01)
        {
            g_ctrlState.ownerVoiceDuration = 0;
            g_ctrlState.ownerVoiceExist    = 0;
            BLE_LOG_D("[SOC_RSP] OWNER_REC_STOP too short duration=%d", durationSec);
            u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, durationSec};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
    }

    BLE_LOG_D("[SOC_RSP] OWNER_REC_STOP unexpected status=0x%02x", payloadLen ? payload[0] : 0xFF);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_owner_rec_play_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1)
    {
        if (payload[0] == 0x00)
        {
            BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY OK");
            u8 rsp[1] = {CTRL_STATUS_OK};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
        if (payload[0] == 0x05)
        {
            BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY no owner voice");
            u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, payload[0]};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
    }

    BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_owner_rec_play_stop_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY_STOP OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY_STOP, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY_STOP failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY_STOP, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_factory_reset_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] FACTORY_RESET OK");
        // Reset MCU cached state to factory defaults (keep powerState).
        g_ctrlState.ownerVoiceExist    = 0;
        g_ctrlState.ownerVoiceDuration = 0;
        g_ctrlState.calmMode           = 0;
        g_ctrlState.enabledMask        = 0x05;
        g_ctrlState.usMask             = 0x07;
        g_ctrlState.measureOrderCount  = 2;
        g_ctrlState.measureOrder[0]    = 1;
        g_ctrlState.measureOrder[1]    = 3;
        g_ctrlState.measureOrder[2]    = 0;
        g_ctrlState.usOrderCount       = 3;
        g_ctrlState.usOrder[0]         = 1;
        g_ctrlState.usOrder[1]         = 2;
        g_ctrlState.usOrder[2]         = 3;

        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_FACTORY_RESET, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] FACTORY_RESET failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_FACTORY_RESET, ctx->bleSeq, rsp, sizeof(rsp));
}

/**
 * @brief  Callback: SOC work state event (0x80).
 *         payload: [workState(1), reason(1)]
 *         Forward to APP as BLE EVENT.
 */
static void app_ctrl_evt_work_state_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    if (payloadLen >= 2)
    {
        g_ctrlState.workState = payload[0];
        BLE_LOG_D("[SOC_EVT] WORK_STATE=%d reason=%d", payload[0], payload[1]);
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_WORK_STATE_CHANGED,
                      g_ctrlSeq++, (u8 *)payload, payloadLen);
    }
}

/**
 * @brief  Callback: SOC session start event (0x81).
 *         payload: [session_id(4LE), bark_ts(4LE)]
 *         Forward to APP as BLE EVENT.
 */
static void app_ctrl_evt_session_start_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    if (payloadLen >= 8)
    {
        u32 session_id = payload[0] | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
        u32 bark_ts    = payload[4] | ((u32)payload[5] << 8) | ((u32)payload[6] << 16) | ((u32)payload[7] << 24);
        BLE_LOG_D("[SOC_EVT] SESSION_START id=%d bark_ts=%d", session_id, bark_ts);
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_SOC_SESSION_START,
                      g_ctrlSeq++, (u8 *)payload, payloadLen);
    }
}

/**
 * @brief  Callback: SOC measure execution event (0x82).
 *         payload: [session_id(4LE), step(1), measure(1), sub(1), ts(4LE)]
 *         Forward to APP as BLE EVENT.
 */
static void app_ctrl_evt_measure_exec_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    if (payloadLen >= 11)
    {
        u32 session_id = payload[0] | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
        BLE_LOG_D("[SOC_EVT] MEASURE_EXEC id=%d step=%d measure=%d sub=%d",
                  session_id, payload[4], payload[5], payload[6]);
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_SOC_MEASURE_EXEC,
                      g_ctrlSeq++, (u8 *)payload, payloadLen);
    }
}

/**
 * @brief  Callback: SOC session result event (0x83).
 *         payload: [session_id(4LE), result(1), end_ts(4LE), ok_measure(1), ok_sub(1)]
 *         Forward to APP as BLE EVENT.
 */
static void app_ctrl_evt_session_result_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    if (payloadLen >= 11)
    {
        u32 session_id = payload[0] | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
        BLE_LOG_D("[SOC_EVT] SESSION_RESULT id=%d result=%d ok_measure=%d ok_sub=%d",
                  session_id, payload[4], payload[9], payload[10]);
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_SOC_SESSION_RESULT,
                      g_ctrlSeq++, (u8 *)payload, payloadLen);
    }
}

/**
 * @brief  Callback: SOC error event (0x86).
 *         payload: [errCode(1), ...]
 *         Forward to APP as BLE EVENT.
 */
static void app_ctrl_evt_soc_error_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    BLE_LOG_D("[SOC_EVT] SOC_ERROR len=%d", payloadLen);
    if (payloadLen >= 1)
    {
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_SOC_ERROR,
                      g_ctrlSeq++, (u8 *)payload, payloadLen);
    }
}

/* 超声波发射状态跟踪 */
_attribute_data_retention_ static u32 g_ultra_stop_tick = 0;

/**
 * @brief  Callback: SOC 通知 MCU 执行超声波发射 (0x68).
 *         payload: [profile(1), duration_sec(1)]
 *           profile=1..3: 启动发射指定时长
 *           profile=0:     停止发射
 */
u8 g_sec = 0;
static void app_ctrl_evt_ultra_emit_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    if (payloadLen < 2)
    {
        return;
    }

    u8 profile = payload[0];
    g_sec = payload[1];

    if (profile == 0)
    {
        /* 停止发射 */
        BLE_LOG_D("[SOC_EVT] ULTRA_EMIT stop");
        app_ultrasonic_stop_emit();
        app_ultrasonic_set_power(0);
        g_ultra_stop_tick = 0;
        return;
    }

    /* 映射 profile → 频率命令 */
    u8 freq_cmd;
    switch (profile)
    {
    case 1:  freq_cmd = 0x01; break;  /* 25KHz */
    case 2:  freq_cmd = 0x02; break;  /* 30KHz */
    case 3:  freq_cmd = 0x03; break;  /* 双频 */
    default:
        BLE_LOG_D("[SOC_EVT] ULTRA_EMIT unknown profile=%u", profile);
        return;
    }

    BLE_LOG_D("[SOC_EVT] ULTRA_EMIT profile=%u freq=0x%02x duration=%us", profile, freq_cmd, g_sec);

    /* 安全限制：单次发射最长 10 秒 */
    if (g_sec > 10) g_sec = 10;

    /* 先停止上一次发射（如有） */
    if (g_ultra_stop_tick != 0)
    {
        app_ultrasonic_stop_emit();
        app_ultrasonic_set_power(0);
        g_ultra_stop_tick = 0;
        sleep_us(50000);
    }

    app_ultrasonic_set_power(1);
    sleep_us(50000);  /* 等待模组上电稳定 */

    if (app_ultrasonic_start_emit(freq_cmd) != 0)
    {
        BLE_LOG_D("[SOC_EVT] ULTRA_EMIT start failed");
        app_ultrasonic_set_power(0);
        return;
    }

    /* 记录停止时间 */
    g_ultra_stop_tick = clock_time();
    BLE_LOG_D("[SOC_EVT] ULTRA_EMIT started, stop at tick=%lu", g_ultra_stop_tick);
}

static void app_ctrl_evt_owner_rec_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)userData;

    // EVT payload: [eventType, bleStatus, durationSec]
    // eventType 0x01 = auto-stop by SOC timeout(10s).
    if (payloadLen < 3 || payload[0] != 0x01)
    {
        return;
    }

    u8 bleStatus   = payload[1];
    u8 durationSec = payload[2];
    if (bleStatus == CTRL_STATUS_OK)
    {
        g_ctrlState.ownerVoiceDuration = durationSec;
        g_ctrlState.ownerVoiceExist    = (durationSec >= 3) ? 1 : 0;
    }
    else
    {
        g_ctrlState.ownerVoiceDuration = 0;
        g_ctrlState.ownerVoiceExist    = 0;
    }

    // Forward to APP as BLE EVENT to indicate recording auto-stop.
    u8 evt_payload[3] = {bleStatus, durationSec, 1};  // 1=auto-stop
    app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_OWNER_REC_STOP, g_ctrlSeq++, evt_payload, sizeof(evt_payload));
}

static void app_ctrl_rsp_time_set_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] TIME_SET OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x01)
    {
        BLE_LOG_D("[SOC_RSP] TIME_SET param error");
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, payload[0]};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] TIME_SET failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_calm_mode_set_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_MODE_SET mode=%d", g_ctrlState.calmMode);
        u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.calmMode};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_SET, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] CALM_MODE_SET failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_SET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_calm_strategy_set_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_SET OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_SET failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_calm_strategy_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 4 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_GET len=%d", payloadLen);
        u8 idx                  = 1;
        g_ctrlState.calmMode    = payload[idx++];
        g_ctrlState.enabledMask = payload[idx++];
        u8 measureCnt           = payload[idx++];
        BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_GET mode=%d enabled=0x%02x mCnt=%d",
                  g_ctrlState.calmMode,
                  g_ctrlState.enabledMask,
                  measureCnt);
        if (measureCnt <= 3 && (u16)(idx + measureCnt + 1) <= payloadLen)
        {
            g_ctrlState.measureOrderCount = measureCnt;
            memset(g_ctrlState.measureOrder, 0, sizeof(g_ctrlState.measureOrder));
            for (u8 i = 0; i < measureCnt; i++)
            {
                g_ctrlState.measureOrder[i] = payload[idx++];
            }

            u8 usCnt = payload[idx++];
            if (usCnt <= 3 && (u16)(idx + usCnt) <= payloadLen)
            {
                g_ctrlState.usOrderCount = usCnt;
                g_ctrlState.usMask       = 0;
                memset(g_ctrlState.usOrder, 0, sizeof(g_ctrlState.usOrder));
                for (u8 i = 0; i < usCnt; i++)
                {
                    g_ctrlState.usOrder[i] = payload[idx++];
                    if (g_ctrlState.usOrder[i] >= 1 && g_ctrlState.usOrder[i] <= 3)
                    {
                        g_ctrlState.usMask |= (u8)(1 << (g_ctrlState.usOrder[i] - 1));
                    }
                }
            }
        }
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
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, ctx->bleSeq, rsp, n);
}

static void app_ctrl_rsp_calm_record_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_record_req_ctx_t *ctx = (app_ctrl_record_req_ctx_t *)userData;
    u8                         entryCnt;
    u8                         remaining;

    if (!ctx || !ctx->busy)
    {
        return;
    }

    if (payloadLen < 3 || payload[0] != 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET failed soc_status=0x%02x len=%d",
                  payloadLen ? payload[0] : 0xFF,
                  payloadLen);
        u8 rsp[3] = {CTRL_STATUS_INTERNAL_ERROR, 0, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, sizeof(rsp));
        ctx->busy = 0;
        return;
    }

    remaining = payload[1]; /* 剩余记录数 */
    entryCnt  = payload[2]; /* 本条记录 entry 数 */

    if (entryCnt == 0)
    {
        /* 无记录 */
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET no records remaining=%d", remaining);
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, sizeof(rsp));
        ctx->busy = 0;
        return;
    }

    /*
     * SOC 响应格式：
     *   payload[0..2] = header (status, remainingCount, entryCount)
     *   后续每条 entry = [type(1B), ts(4B)] = 5B
     *
     * BLE 通知格式（9 字节，每条 entry 一条通知）：
     *   [0] status, [1] remainingRecords, [2] entryIdx,
     *   [3] totalEntriesInRecord, [4] type, [5-8] ts (u32 LE)
     */
    u16 pos = 3;
    for (u8 i = 0; i < entryCnt; i++)
    {
        if ((u16)(pos + 5) > payloadLen)
        {
            BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET entry truncated at %d pos=%d len=%d",
                      i,
                      pos,
                      payloadLen);
            break;
        }

        u8 rsp[9];
        rsp[0] = CTRL_STATUS_OK;
        rsp[1] = remaining;        /* remainingRecords */
        rsp[2] = i;                /* entryIdx */
        rsp[3] = entryCnt;         /* totalEntriesInRecord */
        rsp[4] = payload[pos];     /* type */
        rsp[5] = payload[pos + 1]; /* ts LSB */
        rsp[6] = payload[pos + 2];
        rsp[7] = payload[pos + 3];
        rsp[8] = payload[pos + 4]; /* ts MSB */

        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET entry %d/%d type=0x%02x remaining=%d",
                  i,
                  entryCnt,
                  payload[pos],
                  remaining);
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, sizeof(rsp));
        sleep_us(8000);

        pos += 5;
    }

    ctx->busy = 0;
}

static void app_ctrl_rsp_owner_rec_delete_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1)
    {
        if (payload[0] == 0x00)
        {
            BLE_LOG_D("[SOC_RSP] OWNER_REC_DELETE OK");
            g_ctrlState.ownerVoiceExist    = 0;
            g_ctrlState.ownerVoiceDuration = 0;
            u8 rsp[1]                      = {CTRL_STATUS_OK};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
        else
        {
            BLE_LOG_D("[SOC_RSP] CTRL_STATUS_SOC_ERROR res %d", payload[0]);
            g_ctrlState.ownerVoiceExist    = 0;
            g_ctrlState.ownerVoiceDuration = 0;
            u8 rsp[2]                      = {CTRL_STATUS_SOC_ERROR, payload[0]};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
            return;
        }
    }

    BLE_LOG_D("[SOC_RSP] OWNER_REC_DELETE failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_owner_rec_save_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] OWNER_REC_SAVE OK");
        g_ctrlState.ownerVoiceExist    = 1;
        g_ctrlState.ownerVoiceDuration = (payloadLen >= 2) ? payload[1] : 0;
        u8 rsp[2]                      = {CTRL_STATUS_OK, g_ctrlState.ownerVoiceDuration};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_SAVE, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    BLE_LOG_D("[SOC_RSP] OWNER_REC_SAVE failed status=0x%02x", payloadLen ? payload[0] : 0xFF);
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, payloadLen ? payload[0] : 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_SAVE, ctx->bleSeq, rsp, sizeof(rsp));
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
    sleep_us(10000);
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

static void app_ctrl_rsp_power_ctrl_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
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
        u8 applied_on = payload[1];
        BLE_LOG_D("[SOC_RSP] POWER_CTRL on=%d", applied_on);
        u8 rsp[2] = {CTRL_STATUS_OK, applied_on};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_POWER_CTRL, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] POWER_CTRL failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_POWER_CTRL, ctx->bleSeq, rsp, sizeof(rsp));
    }
}

static int app_ctrl_handle_time_set(u8 seq, u8 *payload, u16 len)
{
    if (len != 5)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_TIME_SET, seq))
    {
        return -1;
    }

    u32 epoch_sec = ((u32)payload[0]) | ((u32)payload[1] << 8) | ((u32)payload[2] << 16) | ((u32)payload[3] << 24);
    s8  tz_q15    = (s8)payload[4];

    g_timeCache.valid    = 1;
    g_timeCache.epochSec = epoch_sec;
    g_timeCache.tzQ15    = tz_q15;
    g_timeCache.lastTick = clock_time();

    g_ctx_time_set.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_TIME_SET,
                                  payload,
                                  len,
                                  app_ctrl_rsp_time_set_from_soc,
                                  &g_ctx_time_set,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_TIME_SET, seq, rsp, sizeof(rsp));
        return -2;
    }

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
    if (!app_ctrl_check_soc_online(CTRL_CMD_POWER_CTRL, seq))
    {
        return -1;
    }

    u8 on = payload[0] ? 1 : 0;
    app_set_power_state(on);
    g_ctrlState.powerState = on;
    g_ctrlState.workState  = on ? 1 : 0;

    g_ctx_power_ctrl.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_POWER_CTRL,
                                  &on,
                                  1,
                                  app_ctrl_rsp_power_ctrl_from_soc,
                                  &g_ctx_power_ctrl,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_POWER_CTRL, seq, rsp, sizeof(rsp));
        return -2;
    }

    BLE_LOG_D("[CTRL] POWER_CTRL on=%d", on);
    return 0;
}

static int app_ctrl_handle_status_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    (void)len;
    if (!app_ctrl_check_soc_online(CTRL_CMD_STATUS_GET, seq))
    {
        return -1;
    }
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
    if (len < 1 || payload[0] > 100)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_VOLUME_SET, seq))
    {
        return -1;
    }
    // 直接传 0-100，SOC 侧做映射
    g_ctx_volume_set.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_VOLUME_SET,
            payload,  /* payload[0] 已是 0-100 */
            1,
            app_ctrl_rsp_volume_set_from_soc,
            &g_ctx_volume_set,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_SET, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static void app_ctrl_rsp_volume_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
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
        // SOC 已返回 0-100，直接使用
        g_ctrlState.volume = payload[1];
        if (g_ctrlState.volume > 100) g_ctrlState.volume = 100;
        BLE_LOG_D("[SOC_RSP] VOLUME_GET pc=%d", g_ctrlState.volume);
        u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.volume};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_GET, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] VOLUME_GET failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_GET, ctx->bleSeq, rsp, sizeof(rsp));
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_VOLUME_GET, seq))
    {
        return -1;
    }
    g_ctx_volume_get.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_STATUS_GET,
                                  0,
                                  0,
                                  app_ctrl_rsp_volume_get_from_soc,
                                  &g_ctx_volume_get,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_VOLUME_GET, seq, rsp, sizeof(rsp));
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_START, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_start.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_START,
            0,
            0,
            app_ctrl_rsp_owner_rec_start_from_soc,
            &g_ctx_owner_rec_start,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_START, seq, rsp, sizeof(rsp));
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_STOP, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_stop.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_STOP,
            0,
            0,
            app_ctrl_rsp_owner_rec_stop_from_soc,
            &g_ctx_owner_rec_stop,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_STOP, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_owner_rec_play(u8 seq, u8 *payload, u16 len)
{
    u8 source;
    if (len == 0)
    {
        source = 0;  /* 空 payload = 播放已保存音频 */
    }
    else if (len == 1 && payload[0] <= 1)
    {
        source = payload[0];
    }
    else
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_PLAY, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_play.bleSeq = seq;
    {
        u8 soc_payload = source;
        if (app_uart_send_cmd_with_cb(
                UART_SOC_OWNER_REC_PLAY,
                &soc_payload,
                1,
                app_ctrl_rsp_owner_rec_play_from_soc,
                &g_ctx_owner_rec_play,
                0) != 0)
        {
            u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY, seq, rsp, sizeof(rsp));
        }
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_DELETE, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_delete.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_DELETE,
            0,
            0,
            app_ctrl_rsp_owner_rec_delete_from_soc,
            &g_ctx_owner_rec_delete,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_DELETE, seq, rsp, sizeof(rsp));
    }
    return 0;
}

static int app_ctrl_handle_owner_rec_play_stop(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY_STOP, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_PLAY_STOP, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_play_stop.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_PLAY_STOP,
            0,
            0,
            app_ctrl_rsp_owner_rec_play_stop_from_soc,
            &g_ctx_owner_rec_play_stop,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_PLAY_STOP, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

static int app_ctrl_handle_owner_rec_save(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_SAVE, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_SAVE, seq))
    {
        return -1;
    }
    g_ctx_owner_rec_save.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(
            UART_SOC_OWNER_REC_SAVE,
            0,
            0,
            app_ctrl_rsp_owner_rec_save_from_soc,
            &g_ctx_owner_rec_save,
            0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_OWNER_REC_SAVE, seq, rsp, sizeof(rsp));
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_OWNER_REC_INFO_GET, seq))
    {
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_MODE_SET, seq))
    {
        return -1;
    }
    g_ctrlState.calmMode       = payload[0];
    g_ctx_calm_mode_set.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_MODE_SET,
                                  payload,
                                  1,
                                  app_ctrl_rsp_calm_mode_set_from_soc,
                                  &g_ctx_calm_mode_set,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MODE_SET, seq, rsp, sizeof(rsp));
        return -2;
    }
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_MODE_GET, seq))
    {
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
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_STRATEGY_SET, seq))
    {
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

    g_ctx_calm_strategy_set.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_STRATEGY_SET,
                                  payload,
                                  len,
                                  app_ctrl_rsp_calm_strategy_set_from_soc,
                                  &g_ctx_calm_strategy_set,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

static int app_ctrl_handle_calm_strategy_get(u8 seq, u8 *payload, u16 len)
{
    u8 mode;

    if (len != 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    mode = payload[0];
    if (mode > 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_STRATEGY_GET, seq))
    {
        return -1;
    }

    g_ctx_calm_strategy_get.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_STRATEGY_GET,
                                  &mode,
                                  1,
                                  app_ctrl_rsp_calm_strategy_get_from_soc,
                                  &g_ctx_calm_strategy_get,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_GET, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

static int app_ctrl_handle_calm_record_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_RECORD_GET, seq))
    {
        return -1;
    }
    if (g_ctx_calm_record_get.busy)
    {
        u8 rsp[2] = {CTRL_STATUS_BUSY, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, seq, rsp, sizeof(rsp));
        return -2;
    }

    memset(&g_ctx_calm_record_get, 0, sizeof(g_ctx_calm_record_get));
    g_ctx_calm_record_get.bleSeq = seq;
    g_ctx_calm_record_get.busy   = 1;

    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_RECORD_GET,
                                  0,
                                  0,
                                  app_ctrl_rsp_calm_record_get_from_soc,
                                  &g_ctx_calm_record_get,
                                  0) != 0)
    {
        g_ctx_calm_record_get.busy = 0;
        u8 rsp[2]                  = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, seq, rsp, sizeof(rsp));
        return -3;
    }
    return 0;
}

static void app_ctrl_rsp_calm_record_delete_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
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
        u8 remaining = payload[1];
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_DELETE OK remaining=%d", remaining);
        u8 rsp[2] = {CTRL_STATUS_OK, remaining};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    BLE_LOG_D("[SOC_RSP] CALM_RECORD_DELETE failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
    u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
}

static int app_ctrl_handle_calm_record_delete(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_RECORD_DELETE, seq))
    {
        return -1;
    }

    g_ctx_calm_record_delete.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_RECORD_DELETE,
                                  0,
                                  0,
                                  app_ctrl_rsp_calm_record_delete_from_soc,
                                  &g_ctx_calm_record_delete,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

static int app_ctrl_handle_factory_reset(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_FACTORY_RESET, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_FACTORY_RESET, seq))
    {
        return -1;
    }

    g_ctx_factory_reset.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_FACTORY_RESET,
                                  payload,
                                  1,
                                  app_ctrl_rsp_factory_reset_from_soc,
                                  &g_ctx_factory_reset,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_FACTORY_RESET, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

// ----------------------- calming music play/stop (forward to SOC) -----------------------
static void app_ctrl_rsp_calm_music_play_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_MUSIC_PLAY OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY, ctx->bleSeq, rsp, sizeof(rsp));
    }
    else
    {
        BLE_LOG_D("[SOC_RSP] CALM_MUSIC_PLAY failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
        u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY, ctx->bleSeq, rsp, sizeof(rsp));
    }
}

static int app_ctrl_handle_calm_music_play(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_MUSIC_PLAY, seq))
    {
        return -1;
    }

    g_ctx_calm_music_play.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_MUSIC_PLAY,
                                  0,
                                  0,
                                  app_ctrl_rsp_calm_music_play_from_soc,
                                  &g_ctx_calm_music_play,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

static void app_ctrl_rsp_calm_music_play_stop_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    if (!ctx)
    {
        return;
    }

    BLE_LOG_D("[SOC_RSP] CALM_MUSIC_PLAY_STOP OK");
    u8 rsp[1] = {CTRL_STATUS_OK};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY_STOP, ctx->bleSeq, rsp, sizeof(rsp));
}

static int app_ctrl_handle_calm_music_play_stop(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY_STOP, seq, rsp, sizeof(rsp));
        return -1;
    }
    if (!app_ctrl_check_soc_online(CTRL_CMD_CALM_MUSIC_PLAY_STOP, seq))
    {
        return -1;
    }

    g_ctx_calm_music_play_stop.bleSeq = seq;
    if (app_uart_send_cmd_with_cb(UART_SOC_CALM_MUSIC_PLAY_STOP,
                                  0,
                                  0,
                                  app_ctrl_rsp_calm_music_play_stop_from_soc,
                                  &g_ctx_calm_music_play_stop,
                                  0) != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_SOC_TIMEOUT, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_MUSIC_PLAY_STOP, seq, rsp, sizeof(rsp));
        return -2;
    }
    return 0;
}

// ----------------------- ultrasonic module (direct MCU GPIO, not via SOC) -----------------------
static int app_ctrl_handle_ultra_cmd(u8 cmdId, u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    (void)len;

    int ret;
    switch (cmdId)
    {
    case CTRL_CMD_ULTRA_SET_25K:
        BLE_LOG_D("[ULTRA] SET_25K");
        ret = app_ultrasonic_cmd_no_data(ULTRA_CMD_25K);
        break;
    case CTRL_CMD_ULTRA_SET_30K:
        BLE_LOG_D("[ULTRA] SET_30K");
        ret = app_ultrasonic_cmd_no_data(ULTRA_CMD_30K);
        break;
    case CTRL_CMD_ULTRA_SET_DUAL:
        BLE_LOG_D("[ULTRA] SET_DUAL");
        ret = app_ultrasonic_cmd_no_data(ULTRA_CMD_DUAL);
        break;
    case CTRL_CMD_ULTRA_TRANS_ON:
        BLE_LOG_D("[ULTRA] TRANS_ON");
        ret = app_ultrasonic_cmd_1byte(ULTRA_CMD_TRANS, 1);
        break;
    case CTRL_CMD_ULTRA_TRANS_OFF:
        BLE_LOG_D("[ULTRA] TRANS_OFF");
        ret = app_ultrasonic_cmd_1byte(ULTRA_CMD_TRANS, 0);
        break;
    case CTRL_CMD_ULTRA_EMIT_ON:
        BLE_LOG_D("[ULTRA] EMIT_ON");
        ret = app_ultrasonic_cmd_1byte(ULTRA_CMD_EMIT, 1);
        break;
    case CTRL_CMD_ULTRA_EMIT_OFF:
        BLE_LOG_D("[ULTRA] EMIT_OFF");
        ret = app_ultrasonic_cmd_1byte(ULTRA_CMD_EMIT, 0);
        break;
    case CTRL_CMD_ULTRA_POWER:
        if (len >= 1) {
            u8 on = payload[0] ? 1 : 0;
            BLE_LOG_D("[ULTRA] POWER %s", on ? "ON" : "OFF");
            app_ultrasonic_set_power(on);
            ret = 0;
        } else {
            ret = -1;
        }
        break;
    default:
        ret = -1;
        break;
    }

    if (ret == 0)
    {
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
    }
    else
    {
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
    }
    return 0;
}

// ----------------------- public APIs -----------------------
void app_ctrl_init(void)
{
    memset(g_ctrlRxBuf, 0, sizeof(g_ctrlRxBuf));
    memset(g_ctrlTxBuf, 0, sizeof(g_ctrlTxBuf));
    g_ctrlSeq                 = 0;
    g_ctrlState.btLinked      = 1;
    g_timeCache.valid         = 0;
    g_soc_online              = 0;
    g_soc_last_heartbeat_tick = 0;
    app_uart_register_evt_handler(UART_SOC_WORK_STATE_EVT,     app_ctrl_evt_work_state_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_OWNER_REC_EVT,      app_ctrl_evt_owner_rec_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_HEARTBEAT_EVT,      app_ctrl_evt_heartbeat_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_BARK_DETECTED_EVT,  app_ctrl_evt_session_start_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_MEASURE_EXEC_EVT,   app_ctrl_evt_measure_exec_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_SESSION_RESULT_EVT, app_ctrl_evt_session_result_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_ERROR_EVT,          app_ctrl_evt_soc_error_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_ULTRA_EMIT_EVT,      app_ctrl_evt_ultra_emit_from_soc, 0);
}

void app_ctrl_time_task(void)
{
    /* 超声波发射超时停止 */
    if (g_ultra_stop_tick != 0 && clock_time_exceed(g_ultra_stop_tick, g_sec * 1000000))
    {
        BLE_LOG_D("[ULTRA] emit timeout, stopping");
        app_ultrasonic_stop_emit();
        app_ultrasonic_set_power(0);
        g_ultra_stop_tick = 0;
    }

    app_ctrl_time_cache_update();

    // Check SOC heartbeat timeout
    if (g_soc_online && clock_time_exceed(g_soc_last_heartbeat_tick, SOC_HEARTBEAT_TIMEOUT_US))
    {
        BLE_LOG_D("SOC heartbeat timeout, mark SOC offline");
        g_soc_online = 0;
    }

    // Poll state changes and push to APP via EVENT
    app_ctrl_state_try_push_event();
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
    case CTRL_CMD_OWNER_REC_PLAY_STOP:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_PLAY_STOP");
        app_ctrl_handle_owner_rec_play_stop(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_SAVE:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_SAVE");
        app_ctrl_handle_owner_rec_save(seq, payload, payLen);
        break;
    case CTRL_CMD_OWNER_REC_INFO_GET:
        BLE_LOG_D("CTRL_CMD_OWNER_REC_INFO_GET");
        app_ctrl_handle_owner_rec_info_get(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_MUSIC_PLAY:
        BLE_LOG_D("CTRL_CMD_CALM_MUSIC_PLAY");
        app_ctrl_handle_calm_music_play(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_MUSIC_PLAY_STOP:
        BLE_LOG_D("CTRL_CMD_CALM_MUSIC_PLAY_STOP");
        app_ctrl_handle_calm_music_play_stop(seq, payload, payLen);
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
    case CTRL_CMD_CALM_RECORD_GET:
        BLE_LOG_D("CTRL_CMD_CALM_RECORD_GET");
        app_ctrl_handle_calm_record_get(seq, payload, payLen);
        break;
    case CTRL_CMD_CALM_RECORD_DELETE:
        BLE_LOG_D("CTRL_CMD_CALM_RECORD_DELETE");
        app_ctrl_handle_calm_record_delete(seq, payload, payLen);
        break;
    case CTRL_CMD_ULTRA_SET_25K:
    case CTRL_CMD_ULTRA_SET_30K:
    case CTRL_CMD_ULTRA_SET_DUAL:
    case CTRL_CMD_ULTRA_TRANS_ON:
    case CTRL_CMD_ULTRA_TRANS_OFF:
    case CTRL_CMD_ULTRA_EMIT_ON:
    case CTRL_CMD_ULTRA_EMIT_OFF:
    case CTRL_CMD_ULTRA_POWER:
        app_ctrl_handle_ultra_cmd(cmdId, seq, payload, payLen);
        break;
    case CTRL_CMD_FACTORY_RESET:
        BLE_LOG_D("CTRL_CMD_FACTORY_RESET");
        app_ctrl_handle_factory_reset(seq, payload, payLen);
        break;
    default: {
        u8 rsp[2] = {CTRL_STATUS_UNSUPPORTED_CMD, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
        break;
    }
    }
}
