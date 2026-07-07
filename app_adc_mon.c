/********************************************************************************************************
 * @file    app_adc_mon.c
 *
 * @brief   电池电压 ADC + NTC 温度 ADC 监测 + 电量管理
 *
 *          从 vendor/ble_cat_laser_toy/app_adc_dbg.c 移植电量管理逻辑：
 *            - Flash 存储电量百分比 (CRC32 校验)
 *            - GPIO 去抖检测充电状态 / USB 插入
 *            - NTC 温控充电开关 (>50°C 关, ≤40°C 开, >70°C 软件关机)
 *            - 电量百分比速率限制 (充/放电不同步进)
 *            - 初始化后 5s 稳定期
 *
 *          电池: PB2, 3.3V 参考, 分压 660k/100k → 实际电压 = ADC_mV * 6.6
 *          NTC:  PB4, 上拉 1kΩ 到 3.3V
 *          CHARGE_STATE: PB7, 低电平充电中
 *          USB_DET:      PD0, 低电平插入 (与 cat_laser_toy 极性相反, 内部取反)
 *          CHARGE_EN:    PB6, 高电平使能充电
 *
 *          NTC 查表与 app_adc_dbg.c 一致 (10 倍电阻, 即 10Ω 单位)
 *
 *******************************************************************************************************/
#include "tl_common.h"
#include "drivers.h"
#include "adc.h"
#include "app_config.h"
#include "app_adc_mon.h"
#include "app_ctrl.h"
#include "app.h"

/* --- 采样参数 --- */
#define APP_ADC_SAMPLE_INTERVAL_US       10000u
#define APP_NTC_CHARGE_OFF_TEMP_C        50
#define APP_NTC_CHARGE_ON_TEMP_C         40
#define APP_NTC_POWER_OFF_TEMP_C         70
#define APP_ADC_REPORT_INTERVAL_US       250000u
#define APP_BAT_DISCHARGE_STEP_S         1u * 1000000u  /* 放电时每秒降 1% */
#define APP_BAT_CHARGE_STEP_S            1u * 1000000u  /* 充电时每秒升 1% */
#define APP_BAT_PERCENT_STABLE_US        5000000u       /* 初始化后 5s 稳定期 */
#define APP_BAT_FLASH_SAVE_INTERVAL_US   30000000u      /* 30s Flash 保存间隔 */
#define APP_BAT_PERCENT_DEFAULT_NO_FLASH 100u
#define APP_CHARGE_GPIO_DEBOUNCE_SAMPLES 2u             /* 10ms * 2 = 20ms，与按键去抖一致 */

#define BAT_PERCENT_FLASH_MAGIC          0x42415450u    /* "BATP" */

/* --- Flash 存储结构 --- */
typedef struct
{
    u32 magic;
    u8  percent;
    u8  reserved[3];
    u32 crc;
} bat_percent_flash_t;

/* --- NTC 查表 (电阻单位 10Ω, 即表中值 *10 = 实际 mΩ) --- */
typedef struct
{
    s8  temp_c;
    u16 res_10ohm;
} ntc_point_t;

static const ntc_point_t g_ntc_table[] = {
    {-30, 11952}, {-29, 11330}, {-28, 10745}, {-27, 10193}, {-26, 9673},
    {-25, 9183},  {-24, 8721},  {-23, 8285},  {-22, 7873},  {-21, 7485},
    {-20, 7118},  {-19, 6771},  {-18, 6443},  {-17, 6133},  {-16, 5840},
    {-15, 5562},  {-14, 5300},  {-13, 5051},  {-12, 4816},  {-11, 4593},
    {-10, 4381},  {-9,  4181},  {-8,  3991},  {-7,  3811},  {-6,  3640},
    {-5,  3477},  {-4,  3323},  {-3,  3177},  {-2,  3038},  {-1,  2905},
    {0,   2780},  {1,   2660},  {2,   2546},  {3,   2438},  {4,   2335},
    {5,   2237},  {6,   2144},  {7,   2055},  {8,   1970},  {9,   1890},
    {10,  1813},  {11,  1739},  {12,  1669},  {13,  1602},  {14,  1539},
    {15,  1478},  {16,  1420},  {17,  1364},  {18,  1311},  {19,  1261},
    {20,  1212},  {21,  1166},  {22,  1122},  {23,  1079},  {24,  1039},
    {25,  1000},  {26,  963},   {27,  927},   {28,  893},   {29,  861},
    {30,  830},   {31,  800},   {32,  771},   {33,  743},   {34,  717},
    {35,  692},   {36,  667},   {37,  644},   {38,  622},   {39,  600},
    {40,  580},   {41,  560},   {42,  541},   {43,  523},   {44,  505},
    {45,  488},   {46,  472},   {47,  457},   {48,  442},   {49,  427},
    {50,  413},   {51,  400},   {52,  387},   {53,  375},   {54,  363},
    {55,  351},   {56,  340},   {57,  330},   {58,  319},   {59,  309},
    {60,  300},   {61,  291},   {62,  282},   {63,  273},   {64,  265},
    {65,  257},   {66,  249},   {67,  242},   {68,  235},   {69,  228},
    {70,  221},   {71,  215},   {72,  209},   {73,  203},   {74,  197},
    {75,  191},   {76,  186},   {77,  180},   {78,  175},   {79,  170},
    {80,  166},   {81,  161},   {82,  157},   {83,  152},   {84,  148},
    {85,  144},   {86,  140},   {87,  137},   {88,  133},   {89,  129},
    {90,  126},   {91,  123},   {92,  119},   {93,  116},   {94,  113},
    {95,  110},   {96,  107},   {97,  105},   {98,  102},   {99,  99},
    {100, 97},    {101, 95},    {102, 92},    {103, 90},    {104, 88},
    {105, 85},    {106, 83},    {107, 81},    {108, 79},    {109, 77},
    {110, 76},    {111, 74},    {112, 72},    {113, 70},    {114, 69},
    {115, 67},    {116, 65},    {117, 64},    {118, 62},    {119, 61},
    {120, 60},    {121, 59},    {122, 57},    {123, 56},    {124, 54},
    {125, 53},
};

/* --- 电池电量查表 (单节锂电) --- */
typedef struct
{
    u16 mv;
    u8  percent;
} bat_point_t;

static const bat_point_t g_bat_table[] = {
    {4200, 100}, {4150, 95}, {4100, 90}, {4060, 85}, {4020, 80},
    {3980, 75},  {3950, 70}, {3920, 65}, {3890, 60}, {3860, 55},
    {3840, 50},  {3820, 45}, {3800, 40}, {3780, 35}, {3760, 30},
    {3740, 25},  {3710, 20}, {3670, 15}, {3630, 10}, {3580, 8},
    {3500, 5},   {3400, 3},  {3200, 1},  {3000, 0},
};

/* --- 状态变量 --- */
static u32 s_adc_sample_tick;
static u32 s_adc_init_tick;
static u32 s_adc_report_tick;
static u32 s_mv_bat_sum;
static u32 s_mv_ntc_sum;
static u16 s_sample_cnt;
static u16 s_bat_mv;
static u8  s_bat_percent;
static u8  s_bat_percent_inited;
static u8  s_bat_prev_charging;
static u32 s_bat_rate_acc_us;
static u8  s_charge_state_stable;
static u8  s_usb_det_stable;
static u8  s_charge_state_cnt;
static u8  s_usb_det_cnt;
static s8  s_ntc_temp_c;
static u8  s_ntc_temp_valid;
static u8  s_charge_switch_on = 1;
static u8  s_ntc_over70_active;
static u32 s_bat_flash_save_tick;
static u8  s_bat_flash_valid;
static u32 s_log_tick;

/* ==================== CRC32 ==================== */
static u32 app_adc_mon_flash_crc32(const u8 *data, u32 len)
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

/* ==================== Flash 电量存储 ==================== */
static u8 app_adc_mon_bat_percent_load_from_flash(u8 *percent_out)
{
    bat_percent_flash_t stored;

    if (!percent_out) return 0;

    flash_read_page(BAT_PERCENT_FLASH_ADDR, sizeof(stored), (u8 *)&stored);
    if (stored.magic != BAT_PERCENT_FLASH_MAGIC) return 0;

    u32 crc = app_adc_mon_flash_crc32((const u8 *)&stored, sizeof(stored) - sizeof(stored.crc));
    if (crc != stored.crc) return 0;
    if (stored.percent > 100) return 0;

    *percent_out = stored.percent;
    return 1;
}

void app_adc_mon_bat_percent_save_to_flash(void)
{
    bat_percent_flash_t stored;
    stored.magic       = BAT_PERCENT_FLASH_MAGIC;
    stored.percent     = s_bat_percent;
    stored.reserved[0] = 0;
    stored.reserved[1] = 0;
    stored.reserved[2] = 0;
    stored.crc         = app_adc_mon_flash_crc32((const u8 *)&stored, sizeof(stored) - sizeof(stored.crc));
    flash_erase_sector(BAT_PERCENT_FLASH_ADDR);
    flash_write_page(BAT_PERCENT_FLASH_ADDR, sizeof(stored), (u8 *)&stored);
}

/* ==================== GPIO 去抖 ==================== */
static void app_adc_mon_gpio_debounce(u8 raw, u8 *stable, u8 *cnt)
{
    if (raw == *stable)
    {
        *cnt = 0;
        return;
    }
    if (++(*cnt) >= APP_CHARGE_GPIO_DEBOUNCE_SAMPLES)
    {
        *stable = raw;
        *cnt    = 0;
    }
}

static void app_adc_mon_charge_gpio_update(void)
{
    u8 charge_raw = gpio_read(GPIO_CHARGE_STATE);
    /* egg_anfu: USB_DET(PD0) 低电平=插入, 高电平=拔出
     * 内部统一用 1=插入, 0=拔出, 所以取反 */
    u8 usb_raw = gpio_read(USB_DET) ? 0 : 1;

    app_adc_mon_gpio_debounce(charge_raw, &s_charge_state_stable, &s_charge_state_cnt);
    app_adc_mon_gpio_debounce(usb_raw, &s_usb_det_stable, &s_usb_det_cnt);
}

/* ==================== NTC 温度查表插值 ==================== */
static int ntc_temp_from_res_10ohm(u16 r_10ohm)
{
    int n = sizeof(g_ntc_table) / sizeof(g_ntc_table[0]);

    if (r_10ohm >= g_ntc_table[0].res_10ohm) return g_ntc_table[0].temp_c;
    if (r_10ohm <= g_ntc_table[n - 1].res_10ohm) return g_ntc_table[n - 1].temp_c;

    for (int i = 0; i < n - 1; i++)
    {
        u16 r_hi = g_ntc_table[i].res_10ohm;
        u16 r_lo = g_ntc_table[i + 1].res_10ohm;
        if (r_10ohm <= r_hi && r_10ohm >= r_lo)
        {
            s8  t_hi   = g_ntc_table[i].temp_c;
            s8  t_lo   = g_ntc_table[i + 1].temp_c;
            s16 dt     = (s16)t_lo - (s16)t_hi;
            u16 dr     = (u16)(r_hi - r_lo);
            u16 offset = (u16)(r_hi - r_10ohm);
            if (dr == 0) return t_hi;
            return (int)((s32)t_hi + (s32)dt * (s32)offset / (s32)dr);
        }
    }
    return g_ntc_table[n - 1].temp_c;
}

/* ==================== 电池百分比查表插值 ==================== */
static u8 bat_percent_from_mv(u16 mv_bat)
{
    int n = (int)(sizeof(g_bat_table) / sizeof(g_bat_table[0]));

    if (mv_bat >= g_bat_table[0].mv)       return g_bat_table[0].percent;
    if (mv_bat <= g_bat_table[n - 1].mv)   return g_bat_table[n - 1].percent;

    for (int i = 0; i < n - 1; i++)
    {
        u16 mv_hi = g_bat_table[i].mv;
        u16 mv_lo = g_bat_table[i + 1].mv;
        if ((mv_bat <= mv_hi) && (mv_bat >= mv_lo))
        {
            u8  p_hi   = g_bat_table[i].percent;
            u8  p_lo   = g_bat_table[i + 1].percent;
            u16 d_mv   = (u16)(mv_hi - mv_lo);
            u16 offset = (u16)(mv_hi - mv_bat);
            if (d_mv == 0) return p_hi;
            u32 delta = (u32)(p_hi - p_lo) * (u32)offset;
            return (u8)(p_hi - (u8)((delta + d_mv / 2u) / d_mv));
        }
    }
    return g_bat_table[n - 1].percent;
}

/* ==================== 电量百分比速率限制 ==================== */
static u8 app_adc_mon_bat_percent_apply_rate_limit(u8 target_percent, u8 is_charging)
{
    u32 step_us = (is_charging ? APP_BAT_CHARGE_STEP_S : APP_BAT_DISCHARGE_STEP_S);

    if (!s_bat_percent_inited)
    {
        s_bat_percent_inited = 1;
        s_bat_prev_charging  = is_charging;
        s_bat_rate_acc_us    = 0;
        return s_bat_percent;
    }

    if (s_bat_prev_charging != is_charging)
    {
        s_bat_prev_charging = is_charging;
        s_bat_rate_acc_us   = 0;
    }

    if (is_charging)
    {
        if (target_percent < s_bat_percent) target_percent = s_bat_percent;
        if (target_percent == s_bat_percent)
        {
            s_bat_rate_acc_us = 0;
            return s_bat_percent;
        }

        s_bat_rate_acc_us += APP_ADC_REPORT_INTERVAL_US;
        while ((s_bat_percent < target_percent) && (s_bat_rate_acc_us >= step_us))
        {
            s_bat_percent++;
            s_bat_rate_acc_us -= step_us;
        }
    }
    else
    {
        if (target_percent > s_bat_percent) target_percent = s_bat_percent;
        if (target_percent == s_bat_percent)
        {
            s_bat_rate_acc_us = 0;
            return s_bat_percent;
        }

        s_bat_rate_acc_us += APP_ADC_REPORT_INTERVAL_US;
        while ((s_bat_percent > target_percent) && (s_bat_rate_acc_us >= step_us))
        {
            s_bat_percent--;
            s_bat_rate_acc_us -= step_us;
        }
    }

    return s_bat_percent;
}

/* ==================== 温控充电管理 ==================== */
static void app_adc_mon_temp_charge_manage(void)
{
    if (!s_ntc_temp_valid) return;

    /* USB 插入且充电开关开启: >50°C 关充电; USB 插入且开关已关: ≤40°C 恢复 */
    if (s_usb_det_stable)
    {
        if (s_charge_switch_on)
        {
            if (s_ntc_temp_c > APP_NTC_CHARGE_OFF_TEMP_C)
            {
                s_charge_switch_on = 0;
                gpio_write(GPIO_CHARGE_EN, 0);
            }
        }
        else if (s_ntc_temp_c <= APP_NTC_CHARGE_ON_TEMP_C)
        {
            s_charge_switch_on = 1;
            gpio_write(GPIO_CHARGE_EN, 1);
        }
    }

    /* >70°C 强制关机 */
    if (s_ntc_temp_c > APP_NTC_POWER_OFF_TEMP_C)
    {
        if (s_charge_switch_on)
        {
            s_charge_switch_on = 0;
            gpio_write(GPIO_CHARGE_EN, 0);
        }

        if (app_get_power_state())
        {
            app_set_power_state(0);
            if (!s_ntc_over70_active)
            {
                s_ntc_over70_active = 1;
                BLE_LOG_D("[ADC_MON] battery temp >70C, forced power off");
            }
        }
    }
    else
    {
        s_ntc_over70_active = 0;
    }
}

/* ==================== 单次 ADC 采样 ==================== */
static u32 app_adc_mon_sample_pin(adc_input_pin_def_e pin)
{
    adc_base_init(pin);
    return adc_sample_and_get_result();
}

/* ==================== 对外 API ==================== */

void app_adc_mon_init(void)
{
    u8 flash_percent = 0;

    adc_init();
    adc_power_on_sar_adc(1);

    s_bat_flash_valid = app_adc_mon_bat_percent_load_from_flash(&flash_percent);
    if (!s_bat_flash_valid)
    {
        flash_percent     = APP_BAT_PERCENT_DEFAULT_NO_FLASH;
        s_bat_flash_valid = 1;
    }

    s_adc_init_tick       = clock_time();
    s_adc_sample_tick     = 0;
    s_adc_report_tick     = 0;
    s_mv_bat_sum          = 0;
    s_mv_ntc_sum          = 0;
    s_sample_cnt          = 0;
    s_bat_mv              = 0;
    s_bat_percent         = flash_percent;
    s_bat_flash_save_tick = 0;
    s_bat_percent_inited  = 0;
    s_bat_prev_charging   = 0xFF;
    s_bat_rate_acc_us     = 0;
    s_charge_state_stable = gpio_read(GPIO_CHARGE_STATE);
    s_usb_det_stable      = gpio_read(USB_DET) ? 0 : 1; /* egg_anfu 低=插入, 取反 */
    s_charge_state_cnt    = 0;
    s_usb_det_cnt         = 0;
    s_ntc_temp_c          = 0;
    s_ntc_temp_valid      = 0;
    s_charge_switch_on    = 1;
    s_ntc_over70_active   = 0;
    s_log_tick            = 0;

    BLE_LOG_D("[ADC_MON] init done (bat=PB2, ntc=PB4, flash_pct=%d)", s_bat_percent);
}

void app_adc_mon_poll(void)
{
    u32 now = clock_time();

    if (s_adc_sample_tick == 0) s_adc_sample_tick = now;
    if (s_adc_report_tick == 0) s_adc_report_tick = now;

    /* --- 10ms 采样 --- */
    if (clock_time_exceed(s_adc_sample_tick, APP_ADC_SAMPLE_INTERVAL_US))
    {
        app_adc_mon_charge_gpio_update();

        u32 mv_bat = app_adc_mon_sample_pin(ADC_GPIO_PB2);
        u32 mv_ntc = app_adc_mon_sample_pin(ADC_GPIO_PB4);

        s_mv_bat_sum += mv_bat;
        s_mv_ntc_sum += mv_ntc;
        s_sample_cnt++;
        s_adc_sample_tick = now;
    }

    /* --- 250ms 上报 --- */
    if (clock_time_exceed(s_adc_report_tick, APP_ADC_REPORT_INTERVAL_US))
    {
        u32 mv_bat_avg = 0;
        u32 mv_ntc_avg = 0;

        if (s_sample_cnt > 0)
        {
            mv_bat_avg = s_mv_bat_sum / s_sample_cnt;
            mv_ntc_avg = s_mv_ntc_sum / s_sample_cnt;
                // BLE_LOG_D("[ADC] mv_bat_avg=%dmV cnt=%d", s_bat_mv, s_sample_cnt);
        }

        /* NTC 温度 */
        s_ntc_temp_valid = 0;
        if (mv_ntc_avg > 0 && mv_ntc_avg < 3300)
        {
            u32 r10       = (1000u * mv_ntc_avg) / (3300u - mv_ntc_avg);
            u16 r_10ohm   = (r10 > 0xFFFFu) ? 0xFFFFu : (u16)r10;
            s_ntc_temp_c  = (s8)ntc_temp_from_res_10ohm(r_10ohm);
            s_ntc_temp_valid = 1;
        }

        /* 电池电压: 分压 660k/100k → 实际电压 = ADC * 6.6 */
        mv_bat_avg = (mv_bat_avg * 66u + 5u) / 10u;

        u8 is_charging      = app_adc_mon_is_charging();
        u8 bat_percent_raw  = bat_percent_from_mv((u16)mv_bat_avg);

        /* ADC 初始化 5s 前使用 Flash 上次电量，避免 raw 不稳定 */
        if (clock_time_exceed(s_adc_init_tick, APP_BAT_PERCENT_STABLE_US))
        {
            u8 bat_percent = app_adc_mon_bat_percent_apply_rate_limit(bat_percent_raw, is_charging);
            s_bat_percent  = bat_percent;

            /* 充电中且电量 <5%: 软件关机 (防止深度放电) */
            if (app_adc_mon_is_charging() && s_bat_percent < 5)
            {
                if (app_get_power_state())
                {
                    BLE_LOG_D("[ADC_MON] bat <5%% while charging, power off");
                    app_set_power_state(0);
                }
            }
        }
        else if (!s_bat_flash_valid)
        {
            BLE_LOG_D("[ADC_MON] Using default bat percent %d%% as flash is invalid", bat_percent_raw);
            s_bat_percent = bat_percent_raw;
        }

        s_bat_mv = (mv_bat_avg > 0xFFFFu) ? 0xFFFFu : (u16)mv_bat_avg;

        /* 每秒日志 */
        // if (s_log_tick == 0 || clock_time_exceed(s_log_tick, 1000000))
        // {
        //     s_log_tick = now;
        //     if (s_ntc_temp_valid)
        //         BLE_LOG_D("[ADC] bat=%dmV(%d) ntc=%dC charge=%d", s_bat_mv, s_bat_percent, s_ntc_temp_c, is_charging);
        //     else
        //         BLE_LOG_D("[ADC] bat=%dmV(%d) ntc=INVALID charge=%d", s_bat_mv, s_bat_percent, is_charging);
        // }

        /* 温控充电管理 */
        app_adc_mon_temp_charge_manage();

        /* 清累加器 */
        s_mv_bat_sum      = 0;
        s_mv_ntc_sum      = 0;
        s_sample_cnt      = 0;
        s_adc_report_tick = now;
    }
}

/* ==================== Getter API ==================== */

u16 app_adc_mon_get_bat_mv(void)
{
    return s_bat_mv;
}

u8 app_adc_mon_get_bat_percent(void)
{
    /* 前端只显示 0/20/40/60/80/100 */
    return s_bat_percent / 20 * 20;
}

u8 app_adc_mon_get_bat_percent_exact(void)
{
    return s_bat_percent;
}

u8 app_adc_mon_is_bat_percent_stable(void)
{
    return (s_bat_percent_inited && clock_time_exceed(s_adc_init_tick, APP_BAT_PERCENT_STABLE_US)) ? 1 : 0;
}

u8 app_adc_mon_is_charging(void)
{
    /* CHARGE_STATE(PB7): 充电中为低电平; USB_DET(PD0): 插入为低电平(已取反, s_usb_det_stable=1=插入) */
    return (s_charge_state_stable == 0 && s_usb_det_stable == 1) ? 1 : 0;
}

u8 app_adc_mon_is_charge_switch_on(void)
{
    return s_charge_switch_on;
}

s8 app_adc_mon_get_ntc_temp(void)
{
    return s_ntc_temp_c;
}

u8 app_adc_mon_is_ntc_valid(void)
{
    return s_ntc_temp_valid;
}
