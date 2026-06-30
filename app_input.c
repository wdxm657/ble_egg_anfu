/********************************************************************************************************
 * @file    app_input.c
 *
 * @brief   GPIO 输入检测模块 — 带软件去抖
 *
 *          去抖算法: 连续采样 N 次一致才确认状态变化
 *          N = DEBOUNCE_MS / POLL_INTERVAL_MS
 *
 *******************************************************************************************************/
#include "tl_common.h"
#include "drivers.h"
#include "app_config.h"
#include "app_input.h"
#include "app_ctrl.h"

/* 去抖参数 */
#define POLL_INTERVAL_US   10000u   /* 10ms 采样一次 */
#define DEBOUNCE_SAMPLES   3u       /* 连续 3 次一致 = 30ms 去抖 */

/* 去抖状态 */
typedef struct
{
    u8 raw;         /* 最新原始值 */
    u8 stable;      /* 去抖后的稳定值 */
    u8 cnt;         /* 连续一致计数器 */
} debounce_t;

static debounce_t s_charge;     /* PB7: 充电状态 */
static debounce_t s_usb;        /* PD0: USB 插入 */
static debounce_t s_key;        /* PA7: 按键 */

static u32 s_poll_tick;

static void debounce_update(debounce_t *d, u8 raw, const char *name)
{
    d->raw = raw;
    if (raw == d->stable)
    {
        d->cnt = 0;
        return;
    }
    if (++d->cnt >= DEBOUNCE_SAMPLES)
    {
        BLE_LOG_D("[INPUT] %s: %d -> %d", name, d->stable, raw);
        d->stable = raw;
        d->cnt    = 0;
    }
}

void app_input_init(void)
{
    /* 硬件 GPIO 已在 app.c user_init_normal 中初始化 */
    s_charge.stable = gpio_read(GPIO_CHARGE_STATE);
    s_usb.stable    = gpio_read(USB_DET);
    s_key.stable    = gpio_read(GPIO_KEY);
    s_poll_tick     = 0;
}

void app_input_poll(void)
{
    u32 now = clock_time();
    if (s_poll_tick == 0)
    {
        s_poll_tick = now;
    }
    if (!clock_time_exceed(s_poll_tick, POLL_INTERVAL_US))
    {
        return;
    }
    s_poll_tick = now;

    debounce_update(&s_charge, gpio_read(GPIO_CHARGE_STATE), "CHARGE");
    debounce_update(&s_usb,    gpio_read(USB_DET),          "USB_DET");
    debounce_update(&s_key,    gpio_read(GPIO_KEY),         "KEY");
}

u8 app_input_is_charging(void)
{
    /* 高电平 = 充电中 (见 app_config.h 注释) */
    return s_charge.stable ? 1 : 0;
}

u8 app_input_is_usb_inserted(void)
{
    /* 高电平 = 已插入 */
    return s_usb.stable ? 1 : 0;
}

u8 app_input_is_key_pressed(void)
{
    /* 低电平 = 按下 */
    return s_key.stable ? 0 : 1;
}
