/*
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef SUPLA_ESP_CLIENT_H_
#define SUPLA_ESP_CLIENT_H_

#include "supla_esp.h"


void DEVCONN_ICACHE_FLASH supla_esp_devconn_init(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_start(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_stop(void);
char * DEVCONN_ICACHE_FLASH supla_esp_devconn_laststate(void);
void DEVCONN_ICACHE_FLASH supla_esp_channel_value_changed(int channel_number, char v);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_send_channel_values_with_delay(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_system_restart(void);

void DEVCONN_ICACHE_FLASH supla_esp_devconn_on_temp_humidity_changed(char humidity);

void DEVCONN_ICACHE_FLASH supla_esp_devconn_before_cfgmode_start(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_before_update_start(void);


#endif /* SUPLA_ESP_CLIENT_H_ */
