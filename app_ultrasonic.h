/********************************************************************************************************
 * @file    app_ultrasonic.h
 *
 * @brief   Ultrasonic module driver — GPIO bit-bang UART (独立于硬件 UART，不干扰 SOC 串口)
 *
 * @details 协议定义见 docs/UART_超声波模组.md
 *          物理层: PD7(TX) PD6(RX), 115200 8N1, GPIO bit-bang
 *
 *******************************************************************************************************/
#ifndef APP_ULTRASONIC_H_
#define APP_ULTRASONIC_H_

#include "tl_common.h"

/* --- 协议常量 --- */
/* 应答状态 */
#define ULTRA_STATUS_OK 0x00

#define ULTRA_HEAD      0xAA

#define ULTRA_CMD_25K   0x01  // 配置 25KHz
#define ULTRA_CMD_30K   0x02  // 配置 30KHz
#define ULTRA_CMD_DUAL  0x03  // 配置 25K+30KHz
#define ULTRA_CMD_TRANS 0x04  // 变压器控制 (DATA[0]=0关 1开)
#define ULTRA_CMD_EMIT  0x05  // 发射控制   (DATA[0]=0停 1发)

/* --- 对外 API --- */

/**
 * @brief     初始化超声波模块 GPIO
 */
void app_ultrasonic_init(void);
void app_ultrasonic_deinit(void);
/**
 * @brief     发送无数据指令（配置频率类）
 * @param cmd ULTRA_CMD_xxx
 * @return    0=成功, -1=超时, -2=响应错误
 */
int app_ultrasonic_cmd_no_data(u8 cmd);

/**
 * @brief     发送带 1 字节数据指令（变压器/发射控制）
 * @return    0=成功, -1=超时, -2=响应错误
 */
int app_ultrasonic_cmd_1byte(u8 cmd, u8 val);

/**
 * @brief     完整开启超声波发射流程：设置频率 → 变压器开 → 发射
 * @param freq_cmd ULTRA_CMD_25K / ULTRA_CMD_30K / ULTRA_CMD_DUAL
 * @return    0=成功
 */
int app_ultrasonic_start_emit(u8 freq_cmd);

/**
 * @brief     停止发射并关闭变压器
 * @return    0=成功
 */
int app_ultrasonic_stop_emit(void);

/**
 * @brief     控制超声波模组供电 (GPIO_ULTAR_EN)
 * @param on  1=开启供电, 0=关闭供电
 */
void app_ultrasonic_set_power(u8 on);

#endif /* APP_ULTRASONIC_H_ */
