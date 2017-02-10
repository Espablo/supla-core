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

#ifndef SUPLA_ESP_CFG_H_
#define SUPLA_ESP_CFG_H_

#include <c_types.h>

#include "supla-dev/proto.h"
#include "supla_esp.h"

#define BTN_TYPE_BUTTON       0
#define BTN_TYPE_SWITCH       1

typedef struct {

	char TAG[6];
	char GUID[SUPLA_GUID_SIZE];
	char Server[SERVER_MAXSIZE];
	int LocationID;
    char LocationPwd[SUPLA_LOCATION_PWD_MAXSIZE];

    char WIFI_SSID[WIFI_SSID_MAXSIZE];
    char WIFI_PWD[WIFI_PWD_MAXSIZE];

    char CfgButtonType;
    char Button1Type;
    char Button2Type;

    char StatusLedOff;
    char InputCfgTriggerOff;

    char FirmwareUpdate;


}SuplaEspCfg;

typedef struct {

	char Relay[RELAY_MAX_COUNT];

/*
	char ltag;
	char len;
	char log[20][200];
*/

}SuplaEspState;

extern SuplaEspCfg supla_esp_cfg;
extern SuplaEspState supla_esp_state;

char CFG_ICACHE_FLASH_ATTR supla_esp_write_state(char *message);
char CFG_ICACHE_FLASH_ATTR supla_esp_read_state(char *message);

char CFG_ICACHE_FLASH_ATTR supla_esp_cfg_init(void);
char CFG_ICACHE_FLASH_ATTR supla_esp_cfg_save(SuplaEspCfg *cfg);
void CFG_ICACHE_FLASH_ATTR supla_esp_save_state(int delay);


//char CFG_ICACHE_FLASH_ATTR supla_esp_write_log(char *log);


#endif /* SUPLA_ESP_CFG_H_ */
