/********************************************************************************************************
 * @file    app_input.h
 *
 * @brief   GPIO 输入检测模块（带软件去抖）
 *
 *          充电状态: PB7, 低电平=充电中, 高电平=充满
 *          USB插入:  PD0, 高电平=已插入
 *          按键:     PA7, 低电平=按下
 *
 *******************************************************************************************************/
#ifndef APP_INPUT_H_
#define APP_INPUT_H_

#include "tl_common.h"

/**
 * @brief     初始化输入 GPIO（已在 app.c 中完成，此处清去抖状态）
 */
void app_input_init(void);

/**
 * @brief     主循环中周期调用，更新去抖状态
 */
void app_input_poll(void);

/**
 * @brief     充电状态 (去抖后)
 * @return    1=充电中, 0=未充电/充满
 */
u8 app_input_is_charging(void);

/**
 * @brief     USB 插入检测 (去抖后)
 * @return    1=已插入, 0=未插入
 */
u8 app_input_is_usb_inserted(void);

/**
 * @brief     按键状态 (去抖后)
 * @return    1=按下, 0=松开
 */
u8 app_input_is_key_pressed(void);

#endif /* APP_INPUT_H_ */
