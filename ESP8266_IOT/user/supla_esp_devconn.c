/*
 ============================================================================
 Name        : supla_esp_devconn.c
 Author      : Przemyslaw Zygmunt przemek@supla.org
 Copyright   : GPLv2
 ============================================================================
*/

#include <string.h>
#include <stdio.h>

#include <user_interface.h>
#include <espconn.h>
#include <spi_flash.h>
#include <osapi.h>
#include <mem.h>

#include "supla_esp_devconn.h"
#include "supla_esp_gpio.h"
#include "supla_esp_cfg.h"
#include "supla_esp_pwm.h"
#include "supla_ds18b20.h"
#include "supla_dht.h"
#include "supla-dev/srpc.h"
#include "supla-dev/log.h"

static ETSTimer supla_devconn_timer1;
static ETSTimer supla_iterate_timer;
static ETSTimer supla_watchdog_timer;

// ESPCONN_INPROGRESS fix
#define SEND_BUFFER_SIZE 500
static char esp_send_buffer[SEND_BUFFER_SIZE];
static char esp_send_buffer_len = 0;
// ---------------------------------------------

#if NOSSL == 1
    #define supla_espconn_sent espconn_sent
    #define supla_espconn_disconnect espconn_disconnect
    #define supla_espconn_connect espconn_connect
#else
    #define supla_espconn_sent espconn_secure_sent
	#define _supla_espconn_disconnect espconn_secure_disconnect
	#define supla_espconn_connect espconn_secure_connect
#endif


static struct espconn ESPConn;
static esp_tcp ESPTCP;
ip_addr_t ipaddr;

static void *srpc = NULL;
static char registered = 0;

static char recvbuff[RECVBUFF_MAXSIZE];
static unsigned int recvbuff_size = 0;

static char devconn_laststate[STATE_MAXSIZE];

static char devconn_autoconnect = 1;

static unsigned int last_response = 0;
static int server_activity_timeout;

static uint8 last_wifi_status = STATION_GOT_IP+1;

void DEVCONN_ICACHE_FLASH supla_esp_devconn_timer1_cb(void *timer_arg);
void DEVCONN_ICACHE_FLASH supla_esp_wifi_check_status(char autoconnect);
void DEVCONN_ICACHE_FLASH supla_esp_srpc_free(void);
void DEVCONN_ICACHE_FLASH supla_esp_devconn_iterate(void *timer_arg);

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_system_restart(void) {

    if ( supla_esp_cfgmode_started() == 0 ) {

    	os_timer_disarm(&supla_watchdog_timer);
    	supla_esp_srpc_free();

		#ifdef BOARD_GPIO_BEFORE_REBOOT
		supla_esp_board_before_reboot();
		#endif

		supla_log(LOG_DEBUG, "RESTART");
    	supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

    	system_restart();


    }
}

int DEVCONN_ICACHE_FLASH
supla_esp_data_read(void *buf, int count, void *dcd) {


	if ( recvbuff_size > 0 ) {

		count = recvbuff_size > count ? count : recvbuff_size;
		os_memcpy(buf, recvbuff, count);

		if ( count == recvbuff_size ) {

			recvbuff_size = 0;

		} else {

			unsigned int a;

			for(a=0;a<recvbuff_size-count;a++)
				recvbuff[a] = recvbuff[count+a];

			recvbuff_size-=count;
		}

		return count;
	}

	return -1;
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_recv_cb (void *arg, char *pdata, unsigned short len) {

	if ( len == 0 || pdata == NULL )
		return;

	if ( len <= RECVBUFF_MAXSIZE-recvbuff_size ) {

		os_memcpy(&recvbuff[recvbuff_size], pdata, len);
		recvbuff_size += len;

		supla_esp_devconn_iterate(NULL);

	} else {
		supla_log(LOG_ERR, "Recv buffer size exceeded");
	}

}

int DEVCONN_ICACHE_FLASH
supla_esp_data_write_append_buffer(void *buf, int count) {

	if ( count > 0 ) {

		if ( esp_send_buffer_len+count > SEND_BUFFER_SIZE ) {

			supla_log(LOG_ERR, "Send buffer size exceeded");
			supla_esp_devconn_system_restart();

			return -1;

		} else {

			memcpy(&esp_send_buffer[esp_send_buffer_len], buf, count);
			esp_send_buffer_len+=count;

			return 0;


		}
	}

	return 0;
}

int DEVCONN_ICACHE_FLASH
supla_esp_data_write(void *buf, int count, void *dcd) {

	int r;

	if ( esp_send_buffer_len > 0
		 && supla_espconn_sent(&ESPConn, esp_send_buffer, esp_send_buffer_len) == 0 ) {

			esp_send_buffer_len = 0;
	};


	if ( esp_send_buffer_len > 0 ) {
		return supla_esp_data_write_append_buffer(buf, count);
	}

	if ( count > 0 ) {

		r = supla_espconn_sent(&ESPConn, buf, count);

		if ( ESPCONN_INPROGRESS == r  ) {
			return supla_esp_data_write_append_buffer(buf, count);
		} else {
			return r == 0 ? count : -1;
		}

	}


	return 0;
}


void DEVCONN_ICACHE_FLASH
supla_esp_set_state(int __pri, const char *message) {

	if ( message == NULL )
		return;

	unsigned char len = strlen(message)+1;

	supla_log(__pri, message);

    if ( len > STATE_MAXSIZE )
    	len = STATE_MAXSIZE;

	os_memcpy(devconn_laststate, message, len);
}

void DEVCONN_ICACHE_FLASH
supla_esp_on_version_error(TSDC_SuplaVersionError *version_error) {

	supla_esp_set_state(LOG_ERR, "Protocol version error");
	supla_esp_devconn_stop();
}


void DEVCONN_ICACHE_FLASH
supla_esp_on_register_result(TSD_SuplaRegisterDeviceResult *register_device_result) {

	switch(register_device_result->result_code) {
	case SUPLA_RESULTCODE_BAD_CREDENTIALS:
		supla_esp_set_state(LOG_ERR, "Bad credentials!");
		break;

	case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
		supla_esp_set_state(LOG_NOTICE, "Temporarily unavailable!");
		break;

	case SUPLA_RESULTCODE_LOCATION_CONFLICT:
		supla_esp_set_state(LOG_ERR, "Location conflict!");
		break;

	case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
		supla_esp_set_state(LOG_ERR, "Channel conflict!");
		break;
	case SUPLA_RESULTCODE_TRUE:

		supla_esp_gpio_state_connected();

		server_activity_timeout = register_device_result->activity_timeout;
		registered = 1;

		supla_esp_set_state(LOG_DEBUG, "Registered and ready.");
		supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

		if ( server_activity_timeout != ACTIVITY_TIMEOUT ) {

			TDCS_SuplaSetActivityTimeout at;
			at.activity_timeout = ACTIVITY_TIMEOUT;
			srpc_dcs_async_set_activity_timeout(srpc, &at);

		}

		//supla_esp_devconn_send_channel_values_with_delay();

		return;

	case SUPLA_RESULTCODE_DEVICE_DISABLED:
		supla_esp_set_state(LOG_NOTICE, "Device is disabled!");
		break;

	case SUPLA_RESULTCODE_LOCATION_DISABLED:
		supla_esp_set_state(LOG_NOTICE, "Location is disabled!");
		break;

	case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
		supla_esp_set_state(LOG_NOTICE, "Device limit exceeded!");
		break;

	case SUPLA_RESULTCODE_GUID_ERROR:
		supla_esp_set_state(LOG_NOTICE, "Incorrect device GUID!");
		break;
	}

	devconn_autoconnect = 0;
	supla_esp_devconn_stop();
}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_set_activity_timeout_result(TSDC_SuplaSetActivityTimeoutResult *result) {
	server_activity_timeout = result->activity_timeout;
}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_value_changed(int channel_number, char v) {

	if ( srpc != NULL
		 && registered == 1 ) {

		//supla_log(LOG_DEBUG, "supla_esp_channel_value_changed(%i, %i)", channel_number, v);

		char value[SUPLA_CHANNELVALUE_SIZE];
		memset(value, 0, SUPLA_CHANNELVALUE_SIZE);
		value[0] = v;

		srpc_ds_async_channel_value_changed(srpc, channel_number, value);
	}

}

#if defined(RGBW_CONTROLLER_CHANNEL) \
    || defined(RGBWW_CONTROLLER_CHANNEL) \
	|| defined(RGB_CONTROLLER_CHANNEL) \
    || defined(DIMMER_CHANNEL)

void DEVCONN_ICACHE_FLASH
supla_esp_channel_rgbw_to_value(char value[SUPLA_CHANNELVALUE_SIZE], int color, char color_brightness, char brightness) {

	memset(value, 0, SUPLA_CHANNELVALUE_SIZE);

	value[0] = brightness;
	value[1] = color_brightness;
	value[2] = (char)((color & 0x000000FF));       // BLUE
	value[3] = (char)((color & 0x0000FF00) >> 8);  // GREEN
	value[4] = (char)((color & 0x00FF0000) >> 16); // RED

}

void DEVCONN_ICACHE_FLASH
supla_esp_channel_rgbw_value_changed(int channel_number, int color, char color_brightness, char brightness) {

	if ( srpc != NULL
		 && registered == 1 ) {

		char value[SUPLA_CHANNELVALUE_SIZE];

		supla_esp_channel_rgbw_to_value(value,color, color_brightness, brightness);
		srpc_ds_async_channel_value_changed(srpc, channel_number, value);
	}

}

#endif

char DEVCONN_ICACHE_FLASH
_supla_esp_channel_set_value(int port, char v, int channel_number) {

	char _v = v == 1 ? HI_VALUE : LO_VALUE;

	supla_esp_gpio_relay_hi(port, _v, 1);

	_v = supla_esp_gpio_relay_is_hi(port);

	supla_esp_channel_value_changed(channel_number, _v == HI_VALUE ? 1 : 0);

	return (v == 1 ? HI_VALUE : LO_VALUE) == _v;
}

void supla_esp_relay_timer_func(void *timer_arg) {

	_supla_esp_channel_set_value(((supla_relay_cfg_t*)timer_arg)->gpio_id, 0, ((supla_relay_cfg_t*)timer_arg)->channel);

}


#if defined(DIMMER_CHANNEL)

	char
	DEVCONN_ICACHE_FLASH supla_esp_channel_set_rgbw_value(int ChannelNumber, int *Color, char *ColorBrightness, char *Brightness) {

		//supla_log(LOG_DEBUG, "Color: %i, CB: %i, B: %i", *Color, *ColorBrightness, *Brightness);

		supla_esp_pwm_set_percent_duty(*Brightness, 100, 0);

		return 1;
	}

#elif defined(RGBW_CONTROLLER_CHANNEL) || defined(RGBWW_CONTROLLER_CHANNEL)

char
DEVCONN_ICACHE_FLASH supla_esp_channel_set_rgbw_value(int ChannelNumber, int *Color, char *ColorBrightness, char *Brightness) {

	//supla_log(LOG_DEBUG, "Color: %i, CB: %i, B: %i", *Color, *ColorBrightness, *Brightness);

	#ifdef COLOR_BRIGHTNESS_PWM

		supla_esp_pwm_set_percent_duty(*Brightness, 100, 0);
		supla_esp_pwm_set_percent_duty(*ColorBrightness, 100, 1);
		supla_esp_pwm_set_percent_duty((((*Color) & 0x00FF0000) >> 16) * 100 / 255, 100, 2); //RED
		supla_esp_pwm_set_percent_duty((((*Color) & 0x0000FF00) >> 8) * 100 / 255, 100, 3);  //GREEN
		supla_esp_pwm_set_percent_duty(((*Color) & 0x000000FF) * 100 / 255, 100, 4);         //BLUE

	#else

		int cn = 255;

		#ifdef RGBW_CONTROLLER_CHANNEL
			cn = RGBW_CONTROLLER_CHANNEL;
		#else
			cn = RGBWW_CONTROLLER_CHANNEL;
		#endif

		if ( ChannelNumber == cn ) {

			char CB = *ColorBrightness;

			if ( CB > 10 && CB < 25 )
				CB = 25;

			if ( CB < 10 )
				CB = 0;

			supla_esp_pwm_set_percent_duty((((*Color) & 0x00FF0000) >> 16) * 100 / 255, CB, 0); //RED
			supla_esp_pwm_set_percent_duty((((*Color) & 0x0000FF00) >> 8) * 100 / 255, CB, 1);  //GREEN
			supla_esp_pwm_set_percent_duty(((*Color) & 0x000000FF) * 100 / 255, CB, 2);         //BLUE

			supla_esp_pwm_set_percent_duty(*Brightness, 100, 3);

		} else if ( ChannelNumber == cn+1 ) {

			#if SUPLA_PWM_COUNT >= 4
				supla_esp_pwm_set_percent_duty(*Brightness, 100, 4);
			#endif

		}



	#endif

	return 1;
}

#elif defined(RGB_CONTROLLER_CHANNEL)

char DEVCONN_ICACHE_FLASH
supla_esp_channel_set_rgbw_value(int ChannelNumber, int *Color, char *ColorBrightness, char *Brightness) {

	//supla_log(LOG_DEBUG, "Color: %i, CB: %i, B: %i", *Color, *ColorBrightness, *Brightness);

	#if SUPLA_PWM_COUNT >= 4

		supla_esp_pwm_set_percent_duty(*ColorBrightness, 100, 0);
		supla_esp_pwm_set_percent_duty((((*Color) & 0x00FF0000) >> 16) * 100 / 255, 100, 1); //RED
		supla_esp_pwm_set_percent_duty((((*Color) & 0x0000FF00) >> 8) * 100 / 255, 100, 2);  //GREEN
		supla_esp_pwm_set_percent_duty(((*Color) & 0x000000FF) * 100 / 255, 100, 3);         //BLUE

	#else

		char CB = *ColorBrightness;

		if ( CB > 10 && CB < 25 )
			CB = 25;

		if ( CB < 10 )
			CB = 0;

		supla_esp_pwm_set_percent_duty((((*Color) & 0x00FF0000) >> 16) * 100 / 255, CB, 0); //RED
		supla_esp_pwm_set_percent_duty((((*Color) & 0x0000FF00) >> 8) * 100 / 255, CB, 1);  //GREEN
		supla_esp_pwm_set_percent_duty(((*Color) & 0x000000FF) * 100 / 255, CB, 2);         //BLUE

	#endif

	return 1;
}

#endif

#if defined(RGB_CONTROLLER_CHANNEL) \
    || defined(RGBW_CONTROLLER_CHANNEL) \
    || defined(RGBWW_CONTROLLER_CHANNEL) \
    || defined(DIMMER_CHANNEL)

char DEVCONN_ICACHE_FLASH
supla_esp_channel_set_rgbw__value(int ChannelNumber, int Color, char ColorBrightness, char Brightness) {
	supla_esp_channel_set_rgbw_value(ChannelNumber, &Color, &ColorBrightness, &Brightness);
}

#endif

void DEVCONN_ICACHE_FLASH
supla_esp_channel_set_value(TSD_SuplaChannelNewValue *new_value) {

#if defined(RGBW_CONTROLLER_CHANNEL) \
	|| defined(RGBWW_CONTROLLER_CHANNEL) \
	|| defined(RGB_CONTROLLER_CHANNEL) \
	|| defined(DIMMER_CHANNEL)

	unsigned char rgb_cn = 255;
	unsigned char dimmer_cn = 255;

    #ifdef RGBW_CONTROLLER_CHANNEL
	rgb_cn = RGBW_CONTROLLER_CHANNEL;
    #endif

	#ifdef RGBWW_CONTROLLER_CHANNEL
	rgb_cn = RGBWW_CONTROLLER_CHANNEL;
	dimmer_cn = RGBWW_CONTROLLER_CHANNEL+1;
	#endif

	#ifdef RGB_CONTROLLER_CHANNEL
	rgb_cn = RGB_CONTROLLER_CHANNEL;
	#endif

	#ifdef DIMMER_CHANNEL
	dimmer_cn = DIMMER_CHANNEL;
	#endif

	if ( new_value->ChannelNumber == rgb_cn
			|| new_value->ChannelNumber == dimmer_cn ) {

		int Color = 0;
		char ColorBrightness = 0;
		char Brightness = 0;

		Brightness = new_value->value[0];
		ColorBrightness = new_value->value[1];

		Color = ((int)new_value->value[4] << 16) & 0x00FF0000; // BLUE
		Color |= ((int)new_value->value[3] << 8) & 0x0000FF00; // GREEN
		Color |= (int)new_value->value[2] & 0x00000FF;         // RED

		if ( Brightness > 100 )
			Brightness = 0;

		if ( ColorBrightness > 100 )
			ColorBrightness = 0;

		supla_esp_channel_set_rgbw_value(new_value->ChannelNumber, &Color, &ColorBrightness, &Brightness);
		supla_esp_channel_rgbw_value_changed(new_value->ChannelNumber, Color, ColorBrightness, Brightness);

		return;
	}

#endif


	char v = new_value->value[0];
	int a;
	char Success = 0;

/*
    char buff[200];
    ets_snprintf(buff, 200, "set_value %i,%i,%i", new_value->value[0], new_value->ChannelNumber, new_value->SenderID);
	supla_esp_write_log(buff);
*/

	for(a=0;a<RELAY_MAX_COUNT;a++)
		if ( supla_relay_cfg[a].gpio_id != 255
			 && new_value->ChannelNumber == supla_relay_cfg[a].channel ) {

			if ( supla_relay_cfg[a].bind != 255 ) {

				char s1, s2, v1, v2;

				v1 = 0;
				v2 = 0;

				if ( v == 1 ) {
					v1 = 1;
					v2 = 0;
				} else if ( v == 2 ) {
					v1 = 0;
					v2 = 1;
				}

				s1 = _supla_esp_channel_set_value(supla_relay_cfg[a].gpio_id, v1, new_value->ChannelNumber);
				s2 = _supla_esp_channel_set_value(supla_relay_cfg[supla_relay_cfg[a].bind].gpio_id, v2, new_value->ChannelNumber);

				Success = s1 != 0 || s2 != 0;

			} else {
				Success = _supla_esp_channel_set_value(supla_relay_cfg[a].gpio_id, v, new_value->ChannelNumber);
			}


			break;
		}


	srpc_ds_async_set_channel_result(srpc, new_value->ChannelNumber, new_value->SenderID, Success);


	if ( v == 1 && new_value->DurationMS > 0 ) {

		for(a=0;a<RELAY_MAX_COUNT;a++)
			if ( supla_relay_cfg[a].gpio_id != 255
				 && new_value->ChannelNumber == supla_relay_cfg[a].channel ) {

				os_timer_disarm(&supla_relay_cfg[a].timer);

				os_timer_setfn(&supla_relay_cfg[a].timer, supla_esp_relay_timer_func, &supla_relay_cfg[a]);
				os_timer_arm (&supla_relay_cfg[a].timer, new_value->DurationMS, false);

				break;
			}

	}

}

void DEVCONN_ICACHE_FLASH
supla_esp_on_remote_call_received(void *_srpc, unsigned int rr_id, unsigned int call_type, void *_dcd, unsigned char proto_version) {

	TsrpcReceivedData rd;
	char result;

	last_response = system_get_time();

	//supla_log(LOG_DEBUG, "call_received");

	if ( SUPLA_RESULT_TRUE == ( result = srpc_getdata(_srpc, &rd, 0)) ) {

		switch(rd.call_type) {
		case SUPLA_SDC_CALL_VERSIONERROR:
			supla_esp_on_version_error(rd.data.sdc_version_error);
			break;
		case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT:
			supla_esp_on_register_result(rd.data.sd_register_device_result);
			break;
		case SUPLA_SD_CALL_CHANNEL_SET_VALUE:
			supla_esp_channel_set_value(rd.data.sd_channel_new_value);
			//supla_esp_devconn_send_channel_values_with_delay();
			break;
		case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:
			supla_esp_channel_set_activity_timeout_result(rd.data.sdc_set_activity_timeout_result);
			break;
		}

		srpc_rd_free(&rd);

	} else if ( result == SUPLA_RESULT_DATA_ERROR ) {

		supla_log(LOG_DEBUG, "DATA ERROR!");
	}

}

void
supla_esp_devconn_iterate(void *timer_arg) {

	if ( srpc != NULL ) {

		if ( registered == 0 ) {
			registered = -1;

			TDS_SuplaRegisterDevice_B srd;
			memset(&srd, 0, sizeof(TDS_SuplaRegisterDevice_B));

			srd.channel_count = 0;
			srd.LocationID = supla_esp_cfg.LocationID;
			ets_snprintf(srd.LocationPWD, SUPLA_LOCATION_PWD_MAXSIZE, "%s", supla_esp_cfg.LocationPwd);

			supla_esp_board_set_device_name(srd.Name, SUPLA_DEVICE_NAME_MAXSIZE);

			strcpy(srd.SoftVer, SUPLA_ESP_SOFTVER);
			os_memcpy(srd.GUID, supla_esp_cfg.GUID, SUPLA_GUID_SIZE);

			//supla_log(LOG_DEBUG, "LocationID=%i, LocationPWD=%s", srd.LocationID, srd.LocationPWD);

			supla_esp_board_set_channels(&srd);

			srpc_ds_async_registerdevice_b(srpc, &srd);

		};

		supla_esp_data_write(NULL, 0, NULL);

		if( srpc_iterate(srpc) == SUPLA_RESULT_FALSE ) {
			supla_log(LOG_DEBUG, "iterate fail");
			supla_esp_devconn_system_restart();
		}

	}

}


void DEVCONN_ICACHE_FLASH
supla_esp_srpc_free(void) {

	os_timer_disarm(&supla_iterate_timer);

	registered = 0;
	last_response = 0;

	if ( srpc != NULL ) {
		srpc_free(srpc);
		srpc = NULL;
	}
}

void DEVCONN_ICACHE_FLASH
supla_esp_srpc_init(void) {
	
	supla_esp_srpc_free();
		
	TsrpcParams srpc_params;
	srpc_params_init(&srpc_params);
	srpc_params.data_read = &supla_esp_data_read;
	srpc_params.data_write = &supla_esp_data_write;
	srpc_params.on_remote_call_received = &supla_esp_on_remote_call_received;

	srpc = srpc_init(&srpc_params);
	
	os_timer_setfn(&supla_iterate_timer, (os_timer_func_t *)supla_esp_devconn_iterate, NULL);
	os_timer_arm(&supla_iterate_timer, 100, 1);

}

void supla_espconn_disconnect(struct espconn *espconn) {
	
	//supla_log(LOG_DEBUG, "Disconnect %i", espconn->state);
	
	if ( espconn->state != ESPCONN_CLOSE
		 && espconn->state != ESPCONN_NONE ) {
		_supla_espconn_disconnect(espconn);
	}
	
}

void
supla_esp_devconn_connect_cb(void *arg) {
	//supla_log(LOG_DEBUG, "devconn_connect_cb\r\n");
	supla_esp_srpc_init();	
	devconn_autoconnect = 1;
}


void
supla_esp_devconn_disconnect_cb(void *arg){
	//supla_log(LOG_DEBUG, "devconn_disconnect_cb\r\n");

	devconn_autoconnect = 1;

	 if ( supla_esp_cfgmode_started() == 0 ) {

			supla_esp_srpc_free();
			supla_esp_wifi_check_status(devconn_autoconnect);

	 }

}


void
supla_esp_devconn_dns_found_cb(const char *name, ip_addr_t *ip, void *arg) {

	if ( ip == NULL ) {
		supla_esp_set_state(LOG_NOTICE, "Domain not found.");
		return;

	}

	supla_espconn_disconnect(&ESPConn);

	ESPConn.proto.tcp = &ESPTCP;
	ESPConn.type = ESPCONN_TCP;
	ESPConn.state = ESPCONN_NONE;

	os_memcpy(ESPConn.proto.tcp->remote_ip, ip, 4);
	ESPConn.proto.tcp->local_port = espconn_port();

	#if NOSSL == 1
		ESPConn.proto.tcp->remote_port = 2015;
	#else
		ESPConn.proto.tcp->remote_port = 2016;
	#endif

	espconn_regist_recvcb(&ESPConn, supla_esp_devconn_recv_cb);
	espconn_regist_connectcb(&ESPConn, supla_esp_devconn_connect_cb);
	espconn_regist_disconcb(&ESPConn, supla_esp_devconn_disconnect_cb);

	supla_espconn_connect(&ESPConn);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_resolvandconnect(void) {

	devconn_autoconnect = 0;

	supla_espconn_disconnect(&ESPConn);

	uint32_t _ip = ipaddr_addr(supla_esp_cfg.Server);

	if ( _ip == -1 ) {
		 supla_log(LOG_DEBUG, "Resolv %s", supla_esp_cfg.Server);

		 espconn_gethostbyname(&ESPConn, supla_esp_cfg.Server, &ipaddr, supla_esp_devconn_dns_found_cb);
	} else {
		 supla_esp_devconn_dns_found_cb(supla_esp_cfg.Server, (ip_addr_t *)&_ip, NULL);
	}


}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_watchdog_cb(void *timer_arg) {

	 if ( supla_esp_cfgmode_started() == 0 ) {

			unsigned int t = system_get_time();

			if ( t > last_response && t-last_response > 60000000 ) {
				supla_log(LOG_DEBUG, "WATCHDOG TIMEOUT");
				supla_esp_devconn_system_restart();
			}

	 }

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_before_cfgmode_start(void) {

	os_timer_disarm(&supla_watchdog_timer);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_init(void) {

	memset(&ESPConn, 0, sizeof(struct espconn));
	memset(&ESPTCP, 0, sizeof(esp_tcp));
	
	last_response = 0;
	devconn_autoconnect = 1;
	ets_snprintf(devconn_laststate, STATE_MAXSIZE, "WiFi - Connecting...");
	//sys_wait_for_restart = 0;

	os_timer_disarm(&supla_watchdog_timer);
	os_timer_setfn(&supla_watchdog_timer, (os_timer_func_t *)supla_esp_devconn_watchdog_cb, NULL);
	os_timer_arm(&supla_watchdog_timer, 1000, 1);

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_start(void) {
	
	wifi_station_disconnect();
	
	supla_esp_gpio_state_disconnected();

    struct station_config stationConf;

    wifi_set_opmode( STATION_MODE );

	#ifdef WIFI_SLEEP_DISABLE
		wifi_set_sleep_type(NONE_SLEEP_T);
	#endif

    os_memcpy(stationConf.ssid, supla_esp_cfg.WIFI_SSID, WIFI_SSID_MAXSIZE);
    os_memcpy(stationConf.password, supla_esp_cfg.WIFI_PWD, WIFI_PWD_MAXSIZE);
   
    stationConf.ssid[31] = 0;
    stationConf.password[63] = 0;
    
    
    wifi_station_set_config(&stationConf);
    wifi_station_set_auto_connect(1);

    wifi_station_connect();
    
	os_timer_disarm(&supla_devconn_timer1);
	os_timer_setfn(&supla_devconn_timer1, (os_timer_func_t *)supla_esp_devconn_timer1_cb, NULL);
	os_timer_arm(&supla_devconn_timer1, 1000, 1);
	
}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_stop(void) {
	
	os_timer_disarm(&supla_devconn_timer1);
	supla_espconn_disconnect(&ESPConn);
	supla_esp_wifi_check_status(0);
}

char * DEVCONN_ICACHE_FLASH
supla_esp_devconn_laststate(void) {
	return devconn_laststate;
}

void DEVCONN_ICACHE_FLASH
supla_esp_wifi_check_status(char autoconnect) {

	uint8 status = wifi_station_get_connect_status();

	if ( status != last_wifi_status )
		supla_log(LOG_DEBUG, "WiFi Status: %i", status);

	last_wifi_status = status;

	if ( STATION_GOT_IP == status ) {

		if ( srpc == NULL ) {

			supla_esp_gpio_state_ipreceived();

			 if ( autoconnect == 1 )
				 supla_esp_devconn_resolvandconnect();
		}


	} else {

		switch(status) {

			case STATION_NO_AP_FOUND:
				supla_esp_set_state(LOG_NOTICE, "SSID Not found");
				break;
			case STATION_WRONG_PASSWORD:
				supla_esp_set_state(LOG_NOTICE, "WiFi - Wrong password");
				break;
		}

		supla_esp_gpio_state_disconnected();

	}

}

void DEVCONN_ICACHE_FLASH
supla_esp_devconn_timer1_cb(void *timer_arg) {

	supla_esp_wifi_check_status(devconn_autoconnect);

	unsigned int t1;
	unsigned int t2;

	//supla_log(LOG_DEBUG, "Free heap size: %i", system_get_free_heap_size());

	if ( registered == 1
		 && server_activity_timeout > 0
		 && srpc != NULL ) {

		    t1 = system_get_time();
		    t2 = abs((t1-last_response)/1000000);

		    if ( t2 >= (server_activity_timeout+10) ) {

		    	supla_log(LOG_DEBUG, "Response timeout %i, %i, %i, %i",  t1, last_response, (t1-last_response)/1000000, server_activity_timeout+5);

		    	supla_esp_srpc_free();
		    	supla_esp_wifi_check_status(devconn_autoconnect);

		    } else if ( t2 >= (server_activity_timeout-5)
		    		    && t2 <= server_activity_timeout ) {

		    	//supla_log(LOG_DEBUG, "PING");
				srpc_dcs_async_ping_server(srpc);

			}

	}
}

#if defined(TEMPERATURE_CHANNEL) || defined(TEMPERATURE_HUMIDITY_CHANNEL)

void DEVCONN_ICACHE_FLASH supla_esp_devconn_on_temp_humidity_changed(char humidity) {

	if ( srpc != NULL
		 && registered == 1 ) {

		char value[SUPLA_CHANNELVALUE_SIZE];

        #if defined(TEMPERATURE_CHANNEL)

		    memset(value, 0, sizeof(SUPLA_CHANNELVALUE_SIZE));

			supla_get_temperature(value);
			srpc_ds_async_channel_value_changed(srpc, TEMPERATURE_CHANNEL, value);

		#endif

        #if defined(TEMPERATURE_HUMIDITY_CHANNEL)

			memset(value, 0, sizeof(SUPLA_CHANNELVALUE_SIZE));

			supla_get_temp_and_humidity(value);
			srpc_ds_async_channel_value_changed(srpc, TEMPERATURE_HUMIDITY_CHANNEL, value);

		#endif


	}

}

#endif



