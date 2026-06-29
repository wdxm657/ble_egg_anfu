/********************************************************************************************************
 * @file    app_ultrasonic.c
 *
 * @brief   Ultrasonic module driver — 基于 SDK software_uart (vendor/common/software_uart.c)
 *
 * @details SDK 软件串口 PD7(TX) PD6(RX) 115200 8N1，使用 Timer0 + GPIO IRQ。
 *          与硬件 UART(SOC) 完全独立，无共享资源。
 *
 *          软串口 RX 数据布局: data[0..3] 保留, data[4..] 为实际收字节
 *
 *******************************************************************************************************/
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "software_uart.h"
#include "app_ultrasonic.h"
#include "app_config.h"
#include "app_ctrl.h"

#define ULTRA_RSP_TIMEOUT_US 150000

/* RX FIFO — 6 字节槽位 (应答最多 5 字节 + 余量)，单 slot 即可装完整应答 */
#define ULTRA_FIFO_SLOT_SIZE 6
#define ULTRA_FIFO_SLOT_NUM  2
static u8 g_ultra_fifo[ULTRA_FIFO_SLOT_SIZE * ULTRA_FIFO_SLOT_NUM];

/* 当前活动的 slot 索引 */
static volatile u8 g_ultra_active_slot;

/* 最近收到的应答数据指针 (指向 g_ultra_fifo 中某个 slot 的 data[4]) */
static volatile u8 *g_ultra_rx_data;
static volatile u8  g_ultra_rx_count;

void app_ultrasonic_set_power(u8 on)
{
    gpio_write(GPIO_ULTAR_EN, on ? 1 : 0);
    if (!on)
    {
        // 串口IO耗电的话在这边测试
    }
}

/**
 * @brief     软串口 RX 回调 — slot 满或被调用时触发
 */
static int app_ultra_soft_rx_cb(void)
{
    extern volatile soft_uart_rece_t soft_uart_rece;

    /* 记录当前 slot 收到的数据 */
    u8 slot          = g_ultra_active_slot;
    g_ultra_rx_count = soft_uart_rece.data[0];                          // data[0] 存放 count
    g_ultra_rx_data  = &g_ultra_fifo[slot * ULTRA_FIFO_SLOT_SIZE + 4];  // 实际字节从偏移 4 开始

    /* 切换到下一个 slot 并重新设置 FIFO */
    g_ultra_active_slot = (slot + 1) & (ULTRA_FIFO_SLOT_NUM - 1);
    u8 *next_slot       = &g_ultra_fifo[g_ultra_active_slot * ULTRA_FIFO_SLOT_SIZE];
    soft_uart_RxSetFifo(next_slot, ULTRA_FIFO_SLOT_SIZE);

    return 0;
}

/**
 * @brief     校验和
 */
static u8 ultra_checksum(const u8 *buf, u8 len)
{
    u8 sum = 0;
    for (u8 i = 0; i < len; i++)
        sum += buf[i];
    return sum;
}

/**
 * @brief     发送指令并等待应答
 */
static int ultra_txrx(u8 cmd, const u8 *data, u8 data_len, u8 *rsp_status)
{
    u8 frame[32];
    u8 frame_len = 3 + data_len;

    /* 组帧 */
    frame[0] = 0xAA;
    frame[1] = cmd;
    frame[2] = data_len;
    if (data_len && data)
        memcpy(&frame[3], data, data_len);
    frame[frame_len] = ultra_checksum(frame, frame_len);
    frame_len++;

    /* 清接收标记 */
    g_ultra_rx_count = 0;
    g_ultra_rx_data  = NULL;

    /* 发送 */
    soft_uart_send(frame, frame_len);
    return 0;
    /* 等待应答 (最多 5 字节) */
    // u32 deadline = clock_time() + ULTRA_RSP_TIMEOUT_US;
    // while (g_ultra_rx_count < 5)
    // {
    //     if (clock_time_exceed(deadline, 0))
    //     {
    //         BLE_LOG_D("[ULTRA] timeout rx_count=%d", g_ultra_rx_count);
    //         return -1;
    //     }
    // }

    // /* 校验应答 */
    // const u8 *resp = g_ultra_rx_data;
    // if (!resp || resp[0] != 0xAA || resp[1] != cmd)
    // {
    //     BLE_LOG_D("[ULTRA] hdr mismatch");
    //     return -2;
    // }
    // u8 check = ultra_checksum(resp, 4);
    // if (resp[4] != check)
    // {
    //     BLE_LOG_D("[ULTRA] chk err: calc=0x%02x recv=0x%02x", check, resp[4]);
    //     return -2;
    // }
    // BLE_LOG_D("suart resp: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x", resp[0], resp[1], resp[2], resp[3], resp[4]);
    // if (rsp_status)
    //     *rsp_status = resp[3];
    // return (resp[3] == 0x00) ? 0 : -3;
}

/* ==================== 对外 API ==================== */

void app_ultrasonic_init(void)
{
    /* 注册 RX 回调 */
    soft_uart_rx_handler(app_ultra_soft_rx_cb);

    /* BLE 协同回调 (防止 BLE 活动干扰软串口时序) */
    extern void blc_sdk_adv(void);
    extern void blc_ll_SoftUartisRfState(void);
    soft_uart_sdk_adv_handler(blc_sdk_adv);
    soft_uart_SoftUartisRfState_handler(blc_ll_SoftUartisRfState);

    /* 设置初始 FIFO slot */
    g_ultra_active_slot = 0;
    soft_uart_RxSetFifo(&g_ultra_fifo[0], ULTRA_FIFO_SLOT_SIZE);

    /* 初始化软串口 */
    soft_uart_init();

    BLE_LOG_D("[ULTRA] soft_uart init done (PD7 TX, PD6 RX, 115200)");
}

int app_ultrasonic_cmd_no_data(u8 cmd)
{
    u8  status;
    int ret = ultra_txrx(cmd, NULL, 0, &status);
    if (ret == 0)
        BLE_LOG_D("[ULTRA] cmd=0x%02x OK st=0x%02x", cmd, status);
    else
        BLE_LOG_D("[ULTRA] cmd=0x%02x fail ret=%d", cmd, ret);
    return ret;
}

int app_ultrasonic_cmd_1byte(u8 cmd, u8 val)
{
    u8  status;
    int ret = ultra_txrx(cmd, &val, 1, &status);
    if (ret == 0)
        BLE_LOG_D("[ULTRA] cmd=0x%02x val=%d OK st=0x%02x", cmd, val, status);
    else
        BLE_LOG_D("[ULTRA] cmd=0x%02x val=%d fail ret=%d", cmd, val, ret);
    return ret;
}

int app_ultrasonic_start_emit(u8 freq_cmd)
{
    if (app_ultrasonic_cmd_no_data(freq_cmd) != 0)
        return -1;
    if (app_ultrasonic_cmd_1byte(0x04, 1) != 0)
        return -2;  // 变压器开
    if (app_ultrasonic_cmd_1byte(0x05, 1) != 0)
        return -3;  // 发射
    return 0;
}

int app_ultrasonic_stop_emit(void)
{
    app_ultrasonic_cmd_1byte(0x05, 0);  // 停止发射
    app_ultrasonic_cmd_1byte(0x04, 0);  // 关闭变压器
    return 0;
}
