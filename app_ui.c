/********************************************************************************************************
 * @file    app_ui.c
 *
 * @brief   This is the source file for BLE SDK
 *
 * @author  BLE GROUP
 * @date    12,2021
 *
 * @par     Copyright (c) 2021, Telink Semiconductor (Shanghai) Co., Ltd. ("TELINK")
 *
 *          Licensed under the Apache License, Version 2.0 (the "License");
 *          you may not use this file except in compliance with the License.
 *          You may obtain a copy of the License at
 *
 *              http://www.apache.org/licenses/LICENSE-2.0
 *
 *          Unless required by applicable law or agreed to in writing, software
 *          distributed under the License is distributed on an "AS IS" BASIS,
 *          WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *          See the License for the specific language governing permissions and
 *          limitations under the License.
 *
 *******************************************************************************************************/
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "application/application.h"

#include "app.h"
#include "app_att.h"
#include "app_ui.h"
#include "app_ctrl.h"

#define LED_BLINK_INTERVAL_US 500000

static u32 g_led_blink_tick = 0;
static u8  g_led_blink_on   = 0;

static void app_ui_led_all_off(void)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_BLUE, !LED_ON_LEVEL);
    gpio_write(GPIO_LED_GREEN, !LED_ON_LEVEL);
    // gpio_write(GPIO_LED_WHITE, !LED_ON_LEVEL);
    gpio_write(GPIO_LED_RED, !LED_ON_LEVEL);
#endif
}

static void app_ui_led_set_blue(u8 on)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_BLUE, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#endif
}

static void app_ui_led_set_green(u8 on)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_GREEN, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#endif
}

static void app_ui_led_set_red(u8 on)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_RED, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#endif
}

static void app_ui_led_set_white(u8 on)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_WHITE, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#endif
}

void app_ui_led_task(void)
{
#if (UI_LED_ENABLE)
    u32 now = clock_time();
#endif
}

#if (UI_KEYBOARD_ENABLE)

int        key_not_released;
u8         key_type;
static u32 keyScanTick = 0;

extern u32 scan_pin_need;

#define CONSUMER_KEY 1
#define KEYBOARD_KEY 2

/**
 * @brief		this function is used to process keyboard matrix status change.
 * @param[in]	none
 * @return      none
 */
void key_change_proc(void)
{
#if (PM_DEEPSLEEP_ENABLE)
    extern u32 latest_user_event_tick;
    latest_user_event_tick = clock_time();  // record latest key change time
#endif

    u8 key0       = kb_event.keycode[0];
    u8 key_buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    key_not_released = 1;
    if (kb_event.cnt == 2)  // two key press, do  not process
    {
        LOG_D("[APP][KEY] Two key press, do not process");
    }
    else if (kb_event.cnt == 1)
    {
        LOG_D("[APP][KEY] One key press, process");
        if (key0 >= CR_VOL_UP)  // volume up/down
        {
            key_type = CONSUMER_KEY;
            u16 consumer_key;
            if (key0 == CR_VOL_UP)
            {  // volume up
                consumer_key = MKEY_VOL_UP;
            }
            else if (key0 == CR_VOL_DN)
            {  // volume down
                consumer_key = MKEY_VOL_DN;
            }
            blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, HID_CONSUME_REPORT_INPUT_DP_H, (u8 *)&consumer_key, 2);
        }
        else
        {
            // normal keyboard key
            key_type   = KEYBOARD_KEY;
            key_buf[2] = key0;
            blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, HID_NORMAL_KB_REPORT_INPUT_DP_H, key_buf, 8);
        }
    }
    else  // kb_event.cnt == 0,  key release
    {
        key_not_released = 0;
        if (key_type == CONSUMER_KEY)
        {
            u16 consumer_key = 0;
            blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, HID_CONSUME_REPORT_INPUT_DP_H, (u8 *)&consumer_key, 2);
        }
        else if (key_type == KEYBOARD_KEY)
        {
            key_buf[2] = 0;
            blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, HID_NORMAL_KB_REPORT_INPUT_DP_H, key_buf, 8);  // release
        }
    }
}

/**
 * @brief      this function is used to detect if key pressed or released.
 * @param[in]  e - LinkLayer Event type
 * @param[in]  p - data pointer of event
 * @param[in]  n - data length of event
 * @return     none
 */

void proc_keyboard(u8 e, u8 *p, int n)
{
    if (clock_time_exceed(keyScanTick, 8000))
    {
        keyScanTick = clock_time();
    }
    else
    {
        return;
    }

    kb_event.keycode[0] = 0;
    // LOG_D("[APP][KEY] Scan key");
    int det_key = kb_scan_key(0, 1);

    if (det_key)
    {
        LOG_D("[APP][KEY] Key press, process");
        key_change_proc();
    }
}

#endif  // end of UI_KEYBOARD_ENABLE

/**
 * @brief      callback function of LinkLayer Event "BLT_EV_FLAG_SUSPEND_ENTER"
 * @param[in]  e - LinkLayer Event type
 * @param[in]  p - data pointer of event
 * @param[in]  n - data length of event
 * @return     none
 */
void task_sleep_enter(u8 e, u8 *p, int n)
{
#if (BLE_APP_PM_ENABLE)
    if (blc_ll_getCurrentState() == BLS_LINK_STATE_CONN && ((u32)(bls_pm_getSystemWakeupTick() - clock_time())) > 80 * CLOCK_16M_SYS_TIMER_CLK_1MS)
    {                                           // suspend time > 30ms.add gpio wakeup
        bls_pm_setWakeupSource(PM_WAKEUP_PAD);  // gpio CORE wakeup suspend
    }
#endif
}

/*----------------------------------------------------------------------------*/
/*------------- OTA  Function                                 ----------------*/
/*----------------------------------------------------------------------------*/
#if (BLE_OTA_SERVER_ENABLE)

_attribute_data_retention_ int ota_is_working = 0;

void app_get_firmware_version(void)
{
    LOG_D("[APP][OTA] Get firmware version");
}

/**
 * @brief      this function is used to register the function for OTA start.
 * @param[in]  none
 * @return     none
 */
void app_enter_ota_mode(void)
{
#if (UI_LED_ENABLE)
    gpio_write(GPIO_LED_BLUE, 1);
    gpio_write(GPIO_LED_GREEN, 1);
#endif
    ota_is_working = 1;
    LOG_D("[APP][OTA] Enter OTA mode");
}

/**
 * @brief       no matter whether the OTA result is successful or fail.
 *              code will run here to tell user the OTA result.
 * @param[in]   result    OTA result:success or fail(different reason)
 * @return      none
 */
void app_ota_result(int result)
{
#if (0)  // this is only for debug, should disable in mass production code
    if (result == OTA_SUCCESS)
    {  // OTA success
        gpio_write(GPIO_LED_BLUE, 0);
        sleep_us(1000000);  // led off for 1 second
        gpio_write(GPIO_LED_BLUE, 1);
        sleep_us(1000000);  // led on for 1 second
    }
    else
    {  // OTA fail

#if 0  // this is only for debug, can not use this in application code
			irq_disable();

			while(1)
			{
				gpio_toggle(GPIO_LED_BLUE);
				sleep_us(1000000);  //led on for 1 second
			}
#endif
    }
#endif
}

#endif
