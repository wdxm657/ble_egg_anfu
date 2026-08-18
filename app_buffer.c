/********************************************************************************************************
 * @file    app_buffer.c
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

#include "app_buffer.h"
#include "app_config.h"



/********************* ACL connection LinkLayer TX & RX data FIFO allocation, Begin ********************************/
u8	app_acl_rxfifo[ACL_RX_FIFO_SIZE * ACL_RX_FIFO_NUM] = {0};
u8	app_acl_txfifo[ACL_TX_FIFO_SIZE * ACL_TX_FIFO_NUM] = {0};
/******************** ACL connection LinkLayer TX & RX data FIFO allocation, End ***********************************/




/***************** ACL connection L2CAP layer MTU TX & RX data FIFO allocation, Begin ********************************/

u8 app_l2cap_rx_fifo[L2CAP_RX_BUFF_SIZE];

/***************** ACL connection L2CAP layer MTU TX & RX data FIFO allocation, End **********************************/


