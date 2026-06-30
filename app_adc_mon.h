/********************************************************************************************************
 * @file    app_adc_mon.h
 *
 * @brief   电池电压 ADC + NTC 温度 ADC 监测模块
 *
 *          电池: PB2, 分压比 660k/100k → 真实电压 = ADC * 6.6
 *          NTC:  PB4, 上拉 3.3V/1kΩ
 *
 *******************************************************************************************************/
#ifndef APP_ADC_MON_H_
#define APP_ADC_MON_H_

#include "tl_common.h"

/**
 * @brief     初始化 ADC 模块
 */
void app_adc_mon_init(void);

/**
 * @brief     主循环中周期调用（10ms 采样, 250ms 上报）
 */
void app_adc_mon_poll(void);

/**
 * @brief     获取电池电压 (mV)
 */
u16 app_adc_mon_get_bat_mv(void);

/**
 * @brief     获取电池电量百分比 (0~100)
 */
u8  app_adc_mon_get_bat_percent(void);

/**
 * @brief     获取 NTC 温度 (°C)
 */
s8  app_adc_mon_get_ntc_temp(void);

/**
 * @brief     NTC 温度是否有效
 */
u8  app_adc_mon_is_ntc_valid(void);

/**
 * @brief     获取精确电池百分比 (0~100, 不做 20 取整)
 */
u8  app_adc_mon_get_bat_percent_exact(void);

/**
 * @brief     电量百分比是否已稳定 (初始化后 5s)
 */
u8  app_adc_mon_is_bat_percent_stable(void);

/**
 * @brief     是否正在充电
 */
u8  app_adc_mon_is_charging(void);

/**
 * @brief     充电开关是否开启
 */
u8  app_adc_mon_is_charge_switch_on(void);

/**
 * @brief     将当前电量百分比保存到 Flash
 */
void app_adc_mon_bat_percent_save_to_flash(void);

#endif /* APP_ADC_MON_H_ */
