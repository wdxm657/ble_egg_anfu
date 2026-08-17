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
#include "app_adc_mon.h"

#include "app.h"

// RX/TX buffers shared with ATT layer
u8 g_ctrlRxBuf[CTRL_RX_MAX_LEN] = {0};
u8 g_ctrlTxBuf[CTRL_TX_MAX_LEN] = {0};

// BLE 连接状态（由 app.c 的 task_connect/task_terminate 更新）
u8 g_ble_connected = 0;

// simple sequence generator for events/async notifications
static u8 g_ctrlSeq = 0;

// Log TX CCC from app_att.c
extern u8 customCtrlLogCCC[2];

// ===================== SOC online tracking via heartbeat =====================
// SOC sends heartbeat every ~2s; if missing for SOC_HEARTBEAT_TIMEOUT_US, mark offline.
#define SOC_HEARTBEAT_TIMEOUT_US 7000000  // 7 s (tolerates ~3 lost beats)

#define CTRL_CALM_MEASURE_MAX         4
#define CTRL_CALM_MEASURE_SNACK_FEED  4
#define CTRL_CALM_MEASURE_MASK_ALL    0x0F
#define CTRL_CALM_MEASURE_MASK_SNACK  (1 << (CTRL_CALM_MEASURE_SNACK_FEED - 1))

/* ===================== 零食奖励开关（flash 持久化，仿 app_adc_mon 电量存储） ===================== */
typedef struct
{
    u32 magic;
    u8  enabled;
    u8  reserved[3];
    u32 crc;
} reward_flag_flash_t;

static u32 app_ctrl_flash_crc32(const u8 *data, u32 len)
{
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (u8 b = 0; b < 8; b++)
        {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/* 从 flash 加载零食奖励开关；无效/无记录返回默认 0（关闭） */
static u8 app_ctrl_reward_flag_load(void)
{
    reward_flag_flash_t stored;

    flash_read_page(REWARD_FLAG_FLASH_ADDR, sizeof(stored), (u8 *)&stored);
    if (stored.magic != REWARD_FLAG_FLASH_MAGIC)
    {
        return 0;
    }
    u32 crc = app_ctrl_flash_crc32((const u8 *)&stored, sizeof(stored) - sizeof(stored.crc));
    if (crc != stored.crc)
    {
        return 0;
    }
    return stored.enabled ? 1 : 0;
}

/* 将零食奖励开关保存到 flash（每次设置时更新，启动时读取恢复） */
static void app_ctrl_reward_flag_save(u8 enabled)
{
    reward_flag_flash_t stored;

    stored.magic       = REWARD_FLAG_FLASH_MAGIC;
    stored.enabled     = enabled ? 1 : 0;
    stored.reserved[0] = 0;
    stored.reserved[1] = 0;
    stored.reserved[2] = 0;
    stored.crc = app_ctrl_flash_crc32((const u8 *)&stored, sizeof(stored) - sizeof(stored.crc));
    flash_erase_sector(REWARD_FLAG_FLASH_ADDR);
    flash_write_page(REWARD_FLAG_FLASH_ADDR, sizeof(stored), (u8 *)&stored);
}

static u8  g_soc_online              = 0;
static u32 g_soc_last_heartbeat_tick = 0;

/* SOC 上线（开机首次心跳 / 重启恢复）后，奖励开关补发窗口次数：每秒 1 次，共 5 秒 */
#define REWARD_SYNC_BOOT_COUNT 5
static u8 g_reward_sync_left = 0;

static void app_ctrl_mark_soc_online(void)
{
    if (!g_soc_online)
    {
        /* SOC 从离线恢复上线：开启 5 秒补发窗口，让 SOC 恢复正确的奖励开关值 */
        g_reward_sync_left = REWARD_SYNC_BOOT_COUNT;
    }
    g_soc_online              = 1;
    g_soc_last_heartbeat_tick = clock_time();
}

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

    if (!g_soc_online)
    {
        BLE_LOG_D("[SOC_EVT] HEARTBEAT");
    }
    
    app_ctrl_mark_soc_online();
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
    u8 enabledMask;         // 安抚措施使能位: bit0=音乐 bit1=主人录音 bit2=超声 bit3=零食投喂
    u8 usMask;              // 超声子措施使能位: bit0=25kHz bit1=30kHz bit2=25&30kHz
    u8 measureOrderCount;   // 安抚措施执行顺序项数(最多 4)
    u8 measureOrder[CTRL_CALM_MEASURE_MAX];     // 安抚措施执行顺序: 1=音乐 2=主人录音 3=超声 4=零食投喂
    u8 usOrderCount;        // 超声执行顺序项数(最多 3)
    u8 usOrder[3];          // 超声执行顺序: 1=25kHz 2=30kHz 3=25&30kHz
    u8 charging;            // 充电状态: 0=未充电, 1=充电中 (由 BLE MCU 本地维护)
    u8 rewardEnabled;       // 零食奖励功能开关: 0=关, 1=开 (flash 持久化, 启动时恢复)
} app_ctrl_state_t;

static app_ctrl_state_t g_ctrlState = {
    .powerState         = 0,
    .workState          = 0,
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

/* 将当前奖励开关同步给 SOC（SET 时立即调用；上线后 5 秒窗口内由 time_task 补发） */
static void app_ctrl_soc_sync_reward_flag(void)
{
    u8 payload[1] = {g_ctrlState.rewardEnabled ? 1 : 0};
    app_uart_send_cmd(UART_SOC_REWARD_FLAG_NOTIFY, payload, 1, NULL);
}

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

/* 上一次推送的状态快照 (10 字节，与 STATUS_GET 响应 payload 一致) */
/* [0]=status always 0, [1]=powerState, [2]=workState, [3]=btLinked, [4]=volume, [5]=calmMode, [6]=enabledMask,
   [7]=usMask, [8]=charging */
static u8 s_pushed_status[9] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0
};

u8 get_work_state(){
   return  g_ctrlState.workState;
}

/**
 * @brief 检查 g_ctrlState + 充电状态是否有变化，有则推送 STATUS 事件给 APP。
 *         payload 格式与 STATUS_GET RSP 一致 (10 字节)。
 *         byte[9]=charging 由 BLE MCU 本地通过 ADC 监测获取。
 */
static void app_ctrl_state_try_push_event(void)
{
    u8 cur[7];
    cur[0] = CTRL_STATUS_OK;
    cur[1] = g_ctrlState.powerState;
    cur[2] = g_ctrlState.workState;
    cur[3] = g_ctrlState.btLinked;
    cur[4] = g_ctrlState.volume;
    cur[5] = g_ctrlState.calmMode;
    // cur[6] = g_ctrlState.enabledMask;
    // cur[7] = g_ctrlState.usMask;
    if (g_ctrlState.workState == 3)
    {
        cur[6] = s_pushed_status[6];
    }else{
        cur[6] = app_adc_mon_is_charging();  /* MCU 本地充电状态 */
    }

    if (memcmp(s_pushed_status, cur, sizeof(cur)) == 0)
    {
        return;  /* 无变化 */
    }

    /* 逐字段对比，仅打印变化的字段 */
    u8 changed = 0;
    if (cur[1] != s_pushed_status[1]) { BLE_LOG_D("  power=%d->%d", s_pushed_status[1], cur[1]); changed = 1; }
    if (cur[2] != s_pushed_status[2]) { BLE_LOG_D("  work=%d->%d", s_pushed_status[2], cur[2]); changed = 1; }
    if (cur[3] != s_pushed_status[3]) { BLE_LOG_D("  bt=%d->%d", s_pushed_status[3], cur[3]); changed = 1; }
    if (cur[4] != s_pushed_status[4]) { BLE_LOG_D("  vol=%d->%d", s_pushed_status[4], cur[4]); changed = 1; }
    if (cur[5] != s_pushed_status[5]) { BLE_LOG_D("  mode=%d->%d", s_pushed_status[5], cur[5]); changed = 1; }
    // if (cur[6] != s_pushed_status[6]) { BLE_LOG_D("  enabledMask=0x%02x->0x%02x", s_pushed_status[6], cur[6]); changed = 1; }
    // if (cur[7] != s_pushed_status[7]) { BLE_LOG_D("  usMask=0x%02x->0x%02x", s_pushed_status[7], cur[7]); changed = 1; }
    if (cur[6] != s_pushed_status[6]) { BLE_LOG_D("  charging=%d->%d", s_pushed_status[6], cur[6]); changed = 1; }

    memcpy(s_pushed_status, cur, sizeof(cur));
    if(changed) {
        app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_STATUS_GET, g_ctrlSeq++, cur, sizeof(cur));
        /* 状态变化时同步触发电池电量上报（走标准 Battery Service 特征）*/
        u8 bat = app_adc_mon_get_bat_percent_exact();
        app_att_battery_update(bat);
    }
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
    u8  bleSeq;
    u8  maxCount;
    u8  totalSend;
    u8  total;
    u8  sent;
    u8  busy;
    u32 busy_tick;  /* clock_time() when busy was set, for timeout */
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
        // 约定: [status,power,work,bt,volume,calmMode,enabledMask,usMask]
        // 注意: powerState 由 BLE MCU 本地维护，不使用 SOC 上报值覆盖。
        if (payload[0] == 0x00)
        {
            g_ctrlState.workState = payload[2];
            // btLinked 为兼容字段，固定由 MCU 侧返回 1，不从 SOC 回包覆盖。
            // SOC 已返回 0-100，直接使用
            g_ctrlState.volume = payload[4];
            if (g_ctrlState.volume > 100) g_ctrlState.volume = 100;
            // g_ctrlState.calmMode        = payload[5];
            // g_ctrlState.enabledMask     = payload[6];
            if (payloadLen >= 8)
            {
                g_ctrlState.usMask = payload[7];
            }
            BLE_LOG_D("[SOC_RSP] STATUS_GET  work=%d vol=%d mode=%d",
                      payload[2],
                      payload[4],
                      payload[5]);
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

    u8 rsp[7] = {
        CTRL_STATUS_OK,
        g_ctrlState.powerState,
        g_ctrlState.workState,
        g_ctrlState.btLinked,
        g_ctrlState.volume,
        g_ctrlState.calmMode,
        // g_ctrlState.enabledMask,
        // g_ctrlState.usMask,
        app_adc_mon_is_charging(),  /* MCU 本地充电状态 */
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
        // 复用 STATUS_GET 回包: [status,power,work,bt,volume,calmMode,enabledMask,usMask]
        g_ctrlState.calmMode = payload[5];
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

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] OWNER_REC_STOP unexpected status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, err};
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

    u8 err = (payloadLen >= 1) ? payload[0] : 0;
    BLE_LOG_D("[SOC_RSP] OWNER_REC_PLAY failed soc_status=0x%02x", err);
    u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, err};
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
    u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, err};
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
        g_ctrlState.measureOrder[3]    = 0;
        g_ctrlState.usOrderCount       = 3;
        g_ctrlState.usOrder[0]         = 1;
        g_ctrlState.usOrder[1]         = 2;
        g_ctrlState.usOrder[2]         = 3;

        /* 恢复出厂：零食奖励开关复位为关闭（清历史偏好） */
        if (g_ctrlState.rewardEnabled != 0)
        {
            g_ctrlState.rewardEnabled = 0;
            app_ctrl_reward_flag_save(0);
            app_ctrl_soc_sync_reward_flag();
        }

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

/**
 * @brief  Callback: SOC new calm record event (0x88).
 *         SOC 完成一条安抚记录并持久化后发送此事件。
 *         如果 BLE 已连接，通过 TEXT_CHUNK 通知上位机有新记录可用。
 *         payload: 无内容。
 */
static void app_ctrl_evt_new_calm_record_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)payload;
    (void)payloadLen;
    (void)userData;

    BLE_LOG_D("[SOC_EVT] NEW_CALM_RECORD");

    if (!g_ble_connected)
    {
        BLE_LOG_D("[SOC_EVT] NEW_CALM_RECORD: BLE not connected, skip notify");
        return;
    }

    /* BLE 已连接，发送文本通知到上位机 */
    app_ctrl_notify_new_record();
}

static void app_ctrl_evt_snack_feed_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    (void)payload;
    (void)payloadLen;
    (void)userData;

    BLE_LOG_D("[SOC_EVT] SNACK_FEED");
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
    u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, err};
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
    u8 rsp[2] = {CTRL_STATUS_SOC_ERROR, err};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, ctx->bleSeq, rsp, sizeof(rsp));
}

static void app_ctrl_rsp_calm_strategy_get_from_soc(u8 cmdId, u8 seq, const u8 *payload, u16 payloadLen, void *userData)
{
    (void)cmdId;
    (void)seq;
    app_ctrl_ble_req_ctx_t *ctx = (app_ctrl_ble_req_ctx_t *)userData;
    u8 rsp_mode = 0;
    if (!ctx)
    {
        return;
    }

    if (payloadLen >= 4 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_GET len=%d", payloadLen);
        u8 idx                  = 1;
        rsp_mode                = payload[idx++];  /* 查询结果中的 mode，仅转发给 APP，不覆盖当前模式 */
        g_ctrlState.enabledMask = payload[idx++];
        u8 measureCnt           = payload[idx++];
        BLE_LOG_D("[SOC_RSP] CALM_STRATEGY_GET mode=%d enabled=0x%02x mCnt=%d",
                  g_ctrlState.calmMode,
                  g_ctrlState.enabledMask,
                  measureCnt);
        if (measureCnt <= CTRL_CALM_MEASURE_MAX && (u16)(idx + measureCnt + 1) <= payloadLen)
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
    rsp[n++] = rsp_mode;
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

    if (!ctx || !ctx->busy)
    {
        return;
    }

    u8                         entryCnt;
    u8                         session_id;

    if (!ctx || !ctx->busy)
    {
        return;
    }

    if (payloadLen < 3 || payload[0] != 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET failed soc_status=0x%02x len=%d",
                  payloadLen ? payload[0] : 0xFF,
                  payloadLen);
        u8 rsp[2] = {CTRL_STATUS_INTERNAL_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, sizeof(rsp));
        ctx->busy = 0;
        return;
    }

    /*
     * SOC 响应格式：
     *   payload[0]   = status
     *   payload[1]   = session_id (1 byte)
     *   payload[2]   = entryCount
     *   payload[3]   = reward (1=本次成功附带零食投喂奖励, 仅结果记录字段)
     *   后续每条 entry = [type(1B), ts(4B)] = 5B
     *
     * BLE 响应格式（9 字节，每条 entry 一条 notify）：
     *   [0] status, [1] entryIdx, [2] totalEntriesInRecord,
     *   [3] session_id (1B),
     *   [4] type, [5-8] ts (u32 LE)
     *   最后一条结果条目（SUCCESS/FAIL）为 10 字节，[9] = reward
     */
    session_id = payload[1];
    entryCnt   = payload[2];

    if (entryCnt == 0)
    {
        /* 无记录 */
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET no records");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, sizeof(rsp));
        ctx->busy = 0;
        return;
    }

    u16 pos = 3;  /* status(1) + session_id(1) + entryCount(1) */
    u8  reward = 0;
    if (payloadLen >= 4)
    {
        reward = payload[3];
        pos    = 4;
    }

    for (u8 i = 0; i < entryCnt; i++)
    {
        if ((u16)(pos + 5) > payloadLen)
        {
            BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET entry truncated at %d pos=%d len=%d",
                      i, pos, payloadLen);
            break;
        }

        u8 rsp[10];
        u8 rlen = 9;
        rsp[0] = CTRL_STATUS_OK;
        rsp[1] = i;                /* entryIdx */
        rsp[2] = entryCnt;         /* totalEntriesInRecord */
        rsp[3] = session_id;       /* session_id (1B) */
        rsp[4] = payload[pos];     /* type */
        rsp[5] = payload[pos + 1]; /* ts LSB */
        rsp[6] = payload[pos + 2];
        rsp[7] = payload[pos + 3];
        rsp[8] = payload[pos + 4]; /* ts MSB */
        if (i == (u8)(entryCnt - 1))
        {
            /* 结果条目（SUCCESS/FAIL）附加零食奖励标记，帧长 10B */
            rsp[9] = reward;
            rlen   = 10;
        }

        BLE_LOG_D("[SOC_RSP] CALM_RECORD_GET entry %d/%d type=0x%02x session_id=%d%s",
                  i, entryCnt, payload[pos], session_id,
                  (rlen == 10) ? (reward ? " reward=1" : " reward=0") : "");
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_GET, ctx->bleSeq, rsp, rlen);
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

// ----------------------- response cache for duplicate seq -----------------------
#define RESP_CACHE_SIZE 4
typedef struct
{
    u8  used;
    u8  frame[CTRL_TX_MAX_LEN];
    u8  len;
} resp_cache_entry_t;

static resp_cache_entry_t g_resp_cache[RESP_CACHE_SIZE];
static u8 g_resp_cache_idx = 0;

static void resp_cache_save(const u8 *frame, u8 len, u8 seq)
{
    resp_cache_entry_t *e = &g_resp_cache[g_resp_cache_idx];
    e->used = 1;
    e->len  = (len <= CTRL_TX_MAX_LEN) ? len : CTRL_TX_MAX_LEN;
    memcpy(e->frame, frame, e->len);
    g_resp_cache_idx = (g_resp_cache_idx + 1) % RESP_CACHE_SIZE;
}

static int resp_cache_resend(u8 cmdId, u8 seq)
{
    for (u8 i = 0; i < RESP_CACHE_SIZE; i++)
    {
        if (g_resp_cache[i].used &&
            g_resp_cache[i].frame[2] == cmdId &&
            g_resp_cache[i].frame[3] == seq)
        {
            if (BLS_CONN_HANDLE != 0xFFFF)
            {
                blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, CUSTOM_COUNTER_READ_DP_H,
                                               g_resp_cache[i].frame, g_resp_cache[i].len);
            }
            return 1;
        }
    }
    return 0;
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
    /* 缓存 RSP 帧用于重复 seq 重发 */
    if (msgType == CTRL_MSG_TYPE_RSP)
    {
        resp_cache_save(g_ctrlTxBuf, (u8)totalLen, seq);
    }
    // memset(g_ctrlRxBuf, 0, sizeof(g_ctrlRxBuf));
    // memset(g_ctrlTxBuf, 0, sizeof(g_ctrlTxBuf));
    return 0;
}

// ----------------------- log output via dedicated Log TX characteristic (0x03 UUID) -----------------------
// Send raw bytes directly via notify on the Log TX handle (no protocol framing).
void app_ctrl_log_send_bytes(const u8 *data, u16 len)
{
    if (!data || len == 0) return;
    if (BLS_CONN_HANDLE == 0xFFFF) return;
    if (!(customCtrlLogCCC[0] & 0x01)) return;  // Notify not enabled by peer

    /* 分片发送，每片最多 20 字节（MTU=23 时 ATT 有效负载上限）*/
    u16 maxChunk = 20;
    u16 offset   = 0;
    while (offset < len)
    {
        u16 chunkLen = (len - offset > maxChunk) ? maxChunk : (u16)(len - offset);
        blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, CUSTOM_COUNTER_LOG_DP_H,
                                       (u8 *)&data[offset], chunkLen);
        offset += chunkLen;
        if (offset < len) sleep_us(5000);
    }
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

static int app_ctrl_handle_reward_flag_set(u8 seq, u8 *payload, u16 len)
{
    if (len < 1)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_REWARD_FLAG_SET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 on = payload[0] ? 1 : 0;
    g_ctrlState.rewardEnabled = on;
    app_ctrl_reward_flag_save(on); /* 本地 flash 持久化，每次启动时恢复 */
    BLE_LOG_D("[CTRL] REWARD_FLAG_SET enabled=%d", on);
    app_ctrl_soc_sync_reward_flag(); /* 立即同步 SOC */

    u8 rsp[2] = {CTRL_STATUS_OK, on};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_REWARD_FLAG_SET, seq, rsp, sizeof(rsp));
    return 0;
}

static int app_ctrl_handle_reward_flag_get(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
    if (len != 0)
    {
        u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_REWARD_FLAG_GET, seq, rsp, sizeof(rsp));
        return -1;
    }

    u8 rsp[2] = {CTRL_STATUS_OK, g_ctrlState.rewardEnabled ? 1 : 0};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_REWARD_FLAG_GET, seq, rsp, sizeof(rsp));
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
        g_ctrlState.volume = payload[4];
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
    if (mode > 1 ||
        enabledMask == 0 ||
        (enabledMask & (u8)~CTRL_CALM_MEASURE_MASK_ALL) ||
        (mode == 0 && (enabledMask & CTRL_CALM_MEASURE_MASK_SNACK)) ||
        measureCnt > CTRL_CALM_MEASURE_MAX)
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
    for (u8 i = 0; i < measureCnt; i++)
    {
        u8 item = payload[idx + i];
        if (item < 1 || item > CTRL_CALM_MEASURE_MAX ||
            (mode == 0 && item == CTRL_CALM_MEASURE_SNACK_FEED))
        {
            u8 rsp[2] = {CTRL_STATUS_PARAM_ERROR, 0};
            app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_STRATEGY_SET, seq, rsp, sizeof(rsp));
            return -1;
        }
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
    g_ctx_calm_record_get.bleSeq    = seq;
    g_ctx_calm_record_get.busy      = 1;
    g_ctx_calm_record_get.busy_tick = clock_time();

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

    if (payloadLen >= 1 && payload[0] == 0x00)
    {
        BLE_LOG_D("[SOC_RSP] CALM_RECORD_DELETE OK");
        u8 rsp[1] = {CTRL_STATUS_OK};
        app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
        return;
    }

    BLE_LOG_D("[SOC_RSP] CALM_RECORD_DELETE failed soc_status=0x%02x", payloadLen ? payload[0] : 0xFF);
    u8 rsp[1] = {CTRL_STATUS_SOC_ERROR};
    app_ctrl_send(CTRL_MSG_TYPE_RSP, CTRL_CMD_CALM_RECORD_DELETE, ctx->bleSeq, rsp, sizeof(rsp));
}

static int app_ctrl_handle_calm_record_delete(u8 seq, u8 *payload, u16 len)
{
    if (len != 1)
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
                                  payload,
                                  1,
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

/**
 * @brief  通知上位机有新的安抚记录可用（通过 TEXT_CHUNK 发送纯文本）。
 *         在以下场景调用：
 *           - BLE 连接成功 (task_connect)
 *           - SOC 完成一条新记录且 BLE 已连接 (DS_EVT_NEW_CALM_RECORD)
 *
 *         上位机收到后自行决定是否调用 CALM_RECORD_GET(0x33) 读取记录。
 */
void app_ctrl_notify_new_record(void)
{
    if (!g_ble_connected)
    {
        return;
    }

    BLE_LOG_D("[NOTIFY] new calm record available");
    u8 evt[1] = {1};  /* recordCount=1, 表示有记录可用 */
    app_ctrl_send(CTRL_MSG_TYPE_EVENT, CTRL_CMD_CALM_RECORD_NOTIFY,
                  g_ctrlSeq++, evt, sizeof(evt));
}

static int app_ctrl_handle_factory_reset(u8 seq, u8 *payload, u16 len)
{
    (void)payload;
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
    /* 每次启动时从 flash 恢复零食奖励开关（无效数据回落为 0=关闭） */
    g_ctrlState.rewardEnabled = app_ctrl_reward_flag_load();
    BLE_LOG_D("[CTRL] REWARD_FLAG loaded from flash=%d", g_ctrlState.rewardEnabled);
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
    app_uart_register_evt_handler(UART_SOC_ULTRA_EMIT_EVT,        app_ctrl_evt_ultra_emit_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_NEW_CALM_RECORD_EVT,   app_ctrl_evt_new_calm_record_from_soc, 0);
    app_uart_register_evt_handler(UART_SOC_SNACK_FEED_EVT,        app_ctrl_evt_snack_feed_from_soc, 0);
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

    /* 每 1 秒向 SOC 同步 MCU 状态（蓝牙连接 + 电源），SOC 重启后可自动恢复*/
    {
        static u32 s_last_sync = 0;
        u32 now = clock_time();
        if (s_last_sync == 0 || clock_time_exceed(s_last_sync, 1000000))
        {
            s_last_sync = now;
            u8 payload[2];
            payload[0] = g_ble_connected;
            payload[1] = g_ctrlState.powerState;
            app_uart_send_cmd(UART_SOC_BT_LINK_NOTIFY, payload, 2, NULL);

            /* 奖励开关仅在 SOC 上线后的 5 秒窗口内每秒补发（开机/SOC 重启恢复用），
             * 窗口结束（5 次）后不再周期性发送；SET 时另有即时同步。 */
            if (g_reward_sync_left > 0)
            {
                app_ctrl_soc_sync_reward_flag();
                g_reward_sync_left--;
            }

            // Poll state changes and push to APP via EVENT
            app_ctrl_state_try_push_event();
        }
    }

    app_ctrl_time_cache_update();

    // Check SOC heartbeat timeout
    if (g_soc_online && clock_time_exceed(g_soc_last_heartbeat_tick, SOC_HEARTBEAT_TIMEOUT_US))
    {
        BLE_LOG_D("SOC heartbeat timeout, mark SOC offline");
        g_soc_online = 0;
    }

    // Clear stuck busy flags (e.g. SOC response was lost)
    if (g_ctx_calm_record_get.busy && clock_time_exceed(g_ctx_calm_record_get.busy_tick, 3000000))
    {
        BLE_LOG_D("[TIMEOUT] CALM_RECORD_GET busy stuck >3s, clearing");
        g_ctx_calm_record_get.busy = 0;
    }
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

    /* 重复 seq 检测：相同 seq 表示 APP 重发，回复上次缓存的响应 */
    {
        static u8 s_last_seq = 0xFF;
        if (seq == s_last_seq)
        {
            BLE_LOG_D("[DUP_SEQ] cmd=0x%02x seq=%d resend cached response", cmdId, seq);
            if (!resp_cache_resend(cmdId, seq))
            {
                /* 缓存未命中，回退到简单的 OK */
                u8 rsp[1] = {CTRL_STATUS_OK};
                app_ctrl_send(CTRL_MSG_TYPE_RSP, cmdId, seq, rsp, sizeof(rsp));
            }
            return;
        }
        s_last_seq = seq;
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
    case CTRL_CMD_REWARD_FLAG_SET:
        BLE_LOG_D("CTRL_CMD_REWARD_FLAG_SET");
        app_ctrl_handle_reward_flag_set(seq, payload, payLen);
        break;
    case CTRL_CMD_REWARD_FLAG_GET:
        BLE_LOG_D("CTRL_CMD_REWARD_FLAG_GET");
        app_ctrl_handle_reward_flag_get(seq, payload, payLen);
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
