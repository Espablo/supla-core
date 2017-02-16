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

#include <os_type.h>

#include "supla_esp.h"


#ifndef SUPLA_ESP_BOARD_H_
#define SUPLA_ESP_BOARD_H_


#if defined(__BOARD_wifisocket)  \
      || defined(__BOARD_wifisocket_x4) \
      || defined(__BOARD_wifisocket_54)

#include "board/wifisocket.h"

#elif defined(__BOARD_gate_module) \
      || defined(__BOARD_gate_module_dht11)    \
      || defined(__BOARD_gate_module_dht22)


#include "board/gate_module.h"

#elif defined(__BOARD_gate_module_wroom)    \
      || defined(__BOARD_gate_module2_wroom)

#include "board/gate_module_wroom.h"

#elif defined(__BOARD_rs_module) \
      || defined(__BOARD_rs_module_wroom)

#include "board/rs_module.h"

#elif defined(__BOARD_sonoff) \
      || defined(__BOARD_sonoff_ds18b20)

#include "board/sonoff.h"

#elif defined(__BOARD_sonoff_touch)

#include "board/sonoff_touch.h"

#elif defined(__BOARD_sonoff_dual)

#include "board/sonoff_dual.h"

#elif defined(__BOARD_sonoff_socket)

#include "board/sonoff_socket.h"

#elif defined(__BOARD_sonoff_th10) \
	  || defined(__BOARD_sonoff_th16)

#include "board/sonoff_th.h"

#elif defined(__BOARD_dimmer)

#elif defined(__BOARD_EgyIOT)

#elif defined(__BOARD_zam_row_01)

#include "board/acs_zam_row_01.h"

#elif defined(__BOARD_zam_sbp_01)

#include "board/acs_zam_sbp_01.h"

#elif defined(__BOARD_ngm)

#include "board/acs_ngm.h"

#elif defined(__BOARD_lightswitch_x2) \
        || defined(__BOARD_lightswitch_x2_DHT11) \
        || defined(__BOARD_lightswitch_x2_DHT22) \
        || defined(__BOARD_lightswitch_x2_54) \
        || defined(__BOARD_lightswitch_x2_54_DHT11) \
        || defined(__BOARD_lightswitch_x2_54_DHT22)

#include "board/lightswitch.h"

#elif defined(__BOARD_inCan)

#include "board/inCan.h"

#elif defined(__BOARD_espablo)

#include "board/espablo.h"

#endif

#endif
