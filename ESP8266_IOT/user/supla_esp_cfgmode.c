/*
 ============================================================================
 Name        : supla_esp_cfgmode.c
 Author      : Przemyslaw Zygmunt przemek@supla.org
 Version     : 1.0
 Copyright   : GPLv2
 ============================================================================
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


#include <ip_addr.h>
#include <user_interface.h>
#include <espconn.h>
#include <spi_flash.h>
#include <osapi.h>
#include <mem.h>

#include "supla_esp.h"
#include "supla_esp_cfg.h"
#include "supla_esp_cfgmode.h"
#include "supla_esp_devconn.h"
#include "supla_esp_gpio.h"

#include "supla-dev/log.h"

#define TYPE_UNKNOWN  0
#define TYPE_GET      1
#define TYPE_POST     2

#define STEP_TYPE        0
#define STEP_GET         2
#define STEP_POST        3
#define STEP_PARSE_VARS  4
#define STEP_DONE        10

#define VAR_NONE         0
#define VAR_SID          1
#define VAR_WPW          2
#define VAR_SVR          3
#define VAR_LID          4
#define VAR_PWD          5
#define VAR_CFGBTN       6
#define VAR_BTN1         7
#define VAR_BTN2         8
#define VAR_ICF          9
#define VAR_LED          10

typedef struct {
	
	char step;
	char type;
	char current_var;
	
	short matched;
	
	char *pbuff;
	int buff_size;
	int offset;
	char intval[12];
	
}TrivialHttpParserVars;


unsigned int supla_esp_cfgmode_entertime = 0;

void ICACHE_FLASH_ATTR 
supla_esp_http_send_response(struct espconn *pespconn, const char *code, const char *html) {

	int html_len = strlen(html);
	char response[] = "HTTP/1.1 %s\r\nAccept-Ranges: bytes\r\nContent-Length: %i\r\nConnection: close\r\nContent-Type: text/html\r\n\r\n%s";
	
	int buff_len = strlen(code)+strlen(response)+html_len+10;
	char *buff = os_malloc(buff_len);
	
	ets_snprintf(buff, buff_len, response, code, html_len, html);

	buff[buff_len-1] = 0;
	espconn_sent(pespconn, buff, strlen(buff));
	os_free(buff);
}

void ICACHE_FLASH_ATTR 
supla_esp_http_ok(struct espconn *pespconn, const char *html) {
	supla_esp_http_send_response(pespconn, "200 OK", html);
}

void ICACHE_FLASH_ATTR 
supla_esp_http_404(struct espconn *pespconn) {
	supla_esp_http_send_response(pespconn, "404 Not Found", "Not Found");
}

void ICACHE_FLASH_ATTR 
supla_esp_http_error(struct espconn *pespconn) {
	supla_esp_http_send_response(pespconn, "500 Internal Server Error", "Error");
}

int ICACHE_FLASH_ATTR
Power(int x, int y) {
	
    int result = 1;
    while (y)
    {
        if (y & 1)
            result *= x;
        
        y >>= 1;
        x *= x;
    }

    return result;
}

int ICACHE_FLASH_ATTR
HexToInt(char *str, int len) {

	int a, n, p;
	int result = 0;

	if ( len%2 != 0 )
		return 0;

	p = len - 1;

	for(a=0;a<len;a++) {
		n = 0;

		if ( str[a] >= 'A' && str[a] <= 'F' )
			n = str[a]-55;
		else if ( str[a] >= 'a' && str[a] <= 'f' )
			n = str[a]-87;
		else if ( str[a] >= '0' && str[a] <= '9' )
			n = str[a]-48;


		result+=Power(16, p)*n;
		p--;
	}

	return result;


};

void ICACHE_FLASH_ATTR
supla_esp_parse_request(TrivialHttpParserVars *pVars, char *pdata, unsigned short len, SuplaEspCfg *cfg) {
	
	if ( len == 0 ) 
		return;
	
	int a, p;
	
	//for(a=0;a<len;a++)
	//	os_printf("%c", pdata[a]);

	if ( pVars->step == STEP_TYPE ) {
		
		char get[] = "GET";
		char post[] = "POST";
		char url[] = " / HTTP";
		
		if ( len >= 3
			 && memcmp(pdata, get, 3) == 0
			 && len >= 10
			 && memcmp(&pdata[3], url, 7) == 0 ) {
			
			pVars->step = STEP_GET;
			pVars->type = TYPE_GET;
			
		} else if ( len >= 4
				 && memcmp(pdata, post, 4) == 0
				 && len >= 11
				 && memcmp(&pdata[4], url, 7) == 0 )  {
			
			pVars->step = STEP_POST;
			pVars->type = TYPE_POST;
		}
		
	}
	
	p = 0;
	
	if ( pVars->step == STEP_POST ) {
		
		char header_end[4] = { '\r', '\n', '\r', '\n' };
		
		for(a=p;a<len;a++) {
		
			if ( len-a >= 4
				 && memcmp(header_end, &pdata[a], 4) == 0 ) {

				pVars->step = STEP_PARSE_VARS;
				p+=3;
			}

		}
		
	}
	
	if ( pVars->step == STEP_PARSE_VARS ) {
		
		for(a=p;a<len;a++) {
		
			if ( pVars->current_var == VAR_NONE ) {
				
				char sid[3] = { 's', 'i', 'd' };
				char wpw[3] = { 'w', 'p', 'w' };
				char svr[3] = { 's', 'v', 'r' };
				char lid[3] = { 'l', 'i', 'd' };
				char pwd[3] = { 'p', 'w', 'd' };
				char btncfg[3] = { 'c', 'f', 'g' };
				char btn1[3] = { 'b', 't', '1' };
				char btn2[3] = { 'b', 't', '2' };
				char icf[3] = { 'i', 'c', 'f' };
				char led[3] = { 'l', 'e', 'd' };
				
				if ( len-a >= 4
					 && pdata[a+3] == '=' ) {

					if ( memcmp(sid, &pdata[a], 3) == 0 ) {
						
						pVars->current_var = VAR_SID;
						pVars->buff_size = WIFI_SSID_MAXSIZE;
						pVars->pbuff = cfg->WIFI_SSID;
						
					} else if ( memcmp(wpw, &pdata[a], 3) == 0 ) {
						
						pVars->current_var = VAR_WPW;
						pVars->buff_size = WIFI_PWD_MAXSIZE;
						pVars->pbuff = cfg->WIFI_PWD;
						
					} else if ( memcmp(svr, &pdata[a], 3) == 0 ) {
						
						pVars->current_var = VAR_SVR;
						pVars->buff_size = SERVER_MAXSIZE;
						pVars->pbuff = cfg->Server;
						
					} else if ( memcmp(lid, &pdata[a], 3) == 0 ) {
						
						pVars->current_var = VAR_LID;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;
						
					} else if ( memcmp(pwd, &pdata[a], 3) == 0 ) {
						
						pVars->current_var = VAR_PWD;
						pVars->buff_size = SUPLA_LOCATION_PWD_MAXSIZE;
						pVars->pbuff = cfg->LocationPwd;
						
					} else if ( memcmp(btncfg, &pdata[a], 3) == 0 ) {

						pVars->current_var = VAR_CFGBTN;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;

					} else if ( memcmp(btn1, &pdata[a], 3) == 0 ) {

						pVars->current_var = VAR_BTN1;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;

					} else if ( memcmp(btn2, &pdata[a], 3) == 0 ) {

						pVars->current_var = VAR_BTN2;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;

					} else if ( memcmp(icf, &pdata[a], 3) == 0 ) {

						pVars->current_var = VAR_ICF;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;

					} else if ( memcmp(led, &pdata[a], 3) == 0 ) {

						pVars->current_var = VAR_LED;
						pVars->buff_size = 12;
						pVars->pbuff = pVars->intval;

					}
					
					a+=4;
					pVars->offset = 0;
				}
				
			}
			
			if ( pVars->current_var != VAR_NONE ) {
				
				if ( pVars->offset < pVars->buff_size
					 && a < len
					 && pdata[a] != '&' ) {
					
					if ( pdata[a] == '%' && a+2 < len ) {
						
						pVars->pbuff[pVars->offset] = HexToInt(&pdata[a+1], 2);
						pVars->offset++;
						a+=2;
						
					} else if ( pdata[a] == '+' ) {

						pVars->pbuff[pVars->offset] = ' ';
						pVars->offset++;

					} else {

						pVars->pbuff[pVars->offset] = pdata[a];
						pVars->offset++;

					}
					
				}

				
				if ( pVars->offset >= pVars->buff_size
					  || a >= len-1
					  || pdata[a] == '&'  ) {
					
					if ( pVars->offset < pVars->buff_size )
						pVars->pbuff[pVars->offset] = 0;
					else
						pVars->pbuff[pVars->buff_size-1] = 0;
					
					
					if ( pVars->current_var == VAR_LID ) {
						
						cfg->LocationID = 0;

						short s=0;
						while(pVars->intval[s]!=0) {

							if ( pVars->intval[s] >= '0' && pVars->intval[s] <= '9' ) {
								cfg->LocationID = cfg->LocationID*10 + pVars->intval[s] - '0';
							}

							s++;
						}
					} else if ( pVars->current_var == VAR_CFGBTN ) {

						cfg->CfgButtonType = pVars->intval[0] - '0';

					} else if ( pVars->current_var == VAR_BTN1 ) {

						cfg->Button1Type = pVars->intval[0] - '0';

					} else if ( pVars->current_var == VAR_BTN2 ) {

						cfg->Button2Type = pVars->intval[0] - '0';

					} else if ( pVars->current_var == VAR_ICF ) {

						cfg->InputCfgTriggerOff = (pVars->intval[0] - '0') == 1 ? 1 : 0;

					} else if ( pVars->current_var == VAR_LED ) {

						cfg->StatusLedOff = (pVars->intval[0] - '0') == 1 ? 1 : 0;
					}
					
					pVars->matched++;
					pVars->current_var = VAR_NONE;

				}
				
			}
			
		}
				
	}
	
}

void ICACHE_FLASH_ATTR
supla_esp_recv_callback (void *arg, char *pdata, unsigned short len)
{	
	struct espconn *conn = (struct espconn *)arg;
	char mac[6];
	char data_saved = 0;

	TrivialHttpParserVars *pVars = (TrivialHttpParserVars *)conn->reverse;

	if ( pdata == NULL || pVars == NULL )
		return;
	
	SuplaEspCfg new_cfg;
	memcpy(&new_cfg, &supla_esp_cfg, sizeof(SuplaEspCfg));
	
	supla_esp_parse_request(pVars, pdata, len, &new_cfg);
	
	if ( pVars->type == TYPE_UNKNOWN ) {
		
		supla_esp_http_404(conn);
		return;
	};
	
	if ( pVars->type == TYPE_POST ) {
		
		//supla_log(LOG_DEBUG, "Matched: %i", pVars->matched);

		if ( pVars->matched < 5 ) {
			return;
		}
				
		if ( new_cfg.LocationPwd[0] == 0 )
			memcpy(new_cfg.LocationPwd, supla_esp_cfg.LocationPwd, SUPLA_LOCATION_PWD_MAXSIZE);
		
		if ( new_cfg.WIFI_PWD[0] == 0 )
					memcpy(new_cfg.WIFI_PWD, supla_esp_cfg.WIFI_PWD, WIFI_PWD_MAXSIZE);
	    
		if ( 1 == supla_esp_cfg_save(&new_cfg) ) {
					
			memcpy(&supla_esp_cfg, &new_cfg, sizeof(SuplaEspCfg));
			data_saved = 1;
			
		} else {
			supla_esp_http_error(conn);
		}
		
	}

	
	if ( false == wifi_get_macaddr(STATION_IF, mac) ) {
		supla_esp_http_error(conn);
		return;
	}

	char dev_name[25];
	supla_esp_board_set_device_name(&dev_name, 25);
	dev_name[24] = 0;
	
	char *buffer = 0;

	#ifdef BOARD_CFG_HTML_TEMPLATE
	    buffer = supla_esp_board_cfg_html_template(dev_name, mac, data_saved);
	#else

        #ifdef CFG_OLD_TEMPLATE
			#ifdef CFGBTN_TYPE_SELECTION

			int bufflen = 830+strlen(dev_name)+strlen(supla_esp_cfg.WIFI_SSID)+strlen(supla_esp_cfg.Server);
			buffer = (char*)os_malloc(bufflen);
		
			ets_snprintf(buffer,
					bufflen,
					"<html><body><h1>%s</h1>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X<br>LAST STATE: %s<br><br><form method=\"post\">WiFi SSID: <input type=\"text\" name=\"sid\" value=\"%s\"><br>WiFi Password: <input type=\"text\" name=\"wpw\" value=\"\"><br><br>Server: <input type=\"text\" name=\"svr\" value=\"%s\"><br>Location ID:<input type=\"number\" name=\"lid\" value=\"%i\"><br>Location password:<input type=\"text\" name=\"pwd\" value=\"\"><br><br>Button type:<select name=\"cfg\"><option value=\"0\" %s>button</option><option value=\"1\" %s>switch</option></select><br><br><input type=\"submit\" value=\"Save\"></form>%s</body></html>",
					dev_name,
					(unsigned char)supla_esp_cfg.GUID[0],
					(unsigned char)supla_esp_cfg.GUID[1],
					(unsigned char)supla_esp_cfg.GUID[2],
					(unsigned char)supla_esp_cfg.GUID[3],
					(unsigned char)supla_esp_cfg.GUID[4],
					(unsigned char)supla_esp_cfg.GUID[5],
					(unsigned char)supla_esp_cfg.GUID[6],
					(unsigned char)supla_esp_cfg.GUID[7],
					(unsigned char)supla_esp_cfg.GUID[8],
					(unsigned char)supla_esp_cfg.GUID[9],
					(unsigned char)supla_esp_cfg.GUID[10],
					(unsigned char)supla_esp_cfg.GUID[11],
					(unsigned char)supla_esp_cfg.GUID[12],
					(unsigned char)supla_esp_cfg.GUID[13],
					(unsigned char)supla_esp_cfg.GUID[14],
					(unsigned char)supla_esp_cfg.GUID[15],
					(unsigned char)mac[0],
					(unsigned char)mac[1],
					(unsigned char)mac[2],
					(unsigned char)mac[3],
					(unsigned char)mac[4],
					(unsigned char)mac[5],
					supla_esp_devconn_laststate(),
					supla_esp_cfg.WIFI_SSID,
					supla_esp_cfg.Server,
					supla_esp_cfg.LocationID,
					supla_esp_cfg.CfgButtonType == BTN_TYPE_BUTTON ? "selected" : "",
					supla_esp_cfg.CfgButtonType == BTN_TYPE_SWITCH ? "selected" : "",
					data_saved == 1 ? "Data saved!" : "");
	
			#elif defined(BTN1_2_TYPE_SELECTION)
	
			ets_snprintf(buffer,
					bufflen,
					"<html><body><h1>%s</h1>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X<br>LAST STATE: %s<br><br><form method=\"post\">WiFi SSID: <input type=\"text\" name=\"sid\" value=\"%s\"><br>WiFi Password: <input type=\"text\" name=\"wpw\" value=\"\"><br><br>Server: <input type=\"text\" name=\"svr\" value=\"%s\"><br>Location ID:<input type=\"number\" name=\"lid\" value=\"%i\"><br>Location password:<input type=\"text\" name=\"pwd\" value=\"\"><br><br>Input1 type:<select name=\"bt1\"><option value=\"0\" %s>button</option><option value=\"1\" %s>switch</option></select><br>Input2 type:<select name=\"bt2\"><option value=\"0\" %s>button</option><option value=\"1\" %s>switch</option></select><br><br><input type=\"submit\" value=\"Save\"></form>%s</body></html>",
					dev_name,
					(unsigned char)supla_esp_cfg.GUID[0],
					(unsigned char)supla_esp_cfg.GUID[1],
					(unsigned char)supla_esp_cfg.GUID[2],
					(unsigned char)supla_esp_cfg.GUID[3],
					(unsigned char)supla_esp_cfg.GUID[4],
					(unsigned char)supla_esp_cfg.GUID[5],
					(unsigned char)supla_esp_cfg.GUID[6],
					(unsigned char)supla_esp_cfg.GUID[7],
					(unsigned char)supla_esp_cfg.GUID[8],
					(unsigned char)supla_esp_cfg.GUID[9],
					(unsigned char)supla_esp_cfg.GUID[10],
					(unsigned char)supla_esp_cfg.GUID[11],
					(unsigned char)supla_esp_cfg.GUID[12],
					(unsigned char)supla_esp_cfg.GUID[13],
					(unsigned char)supla_esp_cfg.GUID[14],
					(unsigned char)supla_esp_cfg.GUID[15],
					(unsigned char)mac[0],
					(unsigned char)mac[1],
					(unsigned char)mac[2],
					(unsigned char)mac[3],
					(unsigned char)mac[4],
					(unsigned char)mac[5],
					supla_esp_devconn_laststate(),
					supla_esp_cfg.WIFI_SSID,
					supla_esp_cfg.Server,
					supla_esp_cfg.LocationID,
					supla_esp_cfg.Button1Type == BTN_TYPE_BUTTON ? "selected" : "",
					supla_esp_cfg.Button1Type == BTN_TYPE_SWITCH ? "selected" : "",
					supla_esp_cfg.Button2Type == BTN_TYPE_BUTTON ? "selected" : "",
					supla_esp_cfg.Button2Type == BTN_TYPE_SWITCH ? "selected" : "",
					data_saved == 1 ? "Data saved!" : "");
	
			#else
	
			ets_snprintf(buffer,
					bufflen,
					"<html><body><h1>%s</h1>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X<br>LAST STATE: %s<br><br><form method=\"post\">WiFi SSID: <input type=\"text\" name=\"sid\" value=\"%s\"><br>WiFi Password: <input type=\"text\" name=\"wpw\" value=\"\"><br><br>Server: <input type=\"text\" name=\"svr\" value=\"%s\"><br>Location ID:<input type=\"number\" name=\"lid\" value=\"%i\"><br>Location password:<input type=\"text\" name=\"pwd\" value=\"\"><br><br><input type=\"submit\" value=\"Save\"></form>%s</body></html>",
					dev_name,
					(unsigned char)supla_esp_cfg.GUID[0],
					(unsigned char)supla_esp_cfg.GUID[1],
					(unsigned char)supla_esp_cfg.GUID[2],
					(unsigned char)supla_esp_cfg.GUID[3],
					(unsigned char)supla_esp_cfg.GUID[4],
					(unsigned char)supla_esp_cfg.GUID[5],
					(unsigned char)supla_esp_cfg.GUID[6],
					(unsigned char)supla_esp_cfg.GUID[7],
					(unsigned char)supla_esp_cfg.GUID[8],
					(unsigned char)supla_esp_cfg.GUID[9],
					(unsigned char)supla_esp_cfg.GUID[10],
					(unsigned char)supla_esp_cfg.GUID[11],
					(unsigned char)supla_esp_cfg.GUID[12],
					(unsigned char)supla_esp_cfg.GUID[13],
					(unsigned char)supla_esp_cfg.GUID[14],
					(unsigned char)supla_esp_cfg.GUID[15],
					(unsigned char)mac[0],
					(unsigned char)mac[1],
					(unsigned char)mac[2],
					(unsigned char)mac[3],
					(unsigned char)mac[4],
					(unsigned char)mac[5],
					supla_esp_devconn_laststate(),
					supla_esp_cfg.WIFI_SSID,
					supla_esp_cfg.Server,
					supla_esp_cfg.LocationID,
					data_saved == 1 ? "Data saved!" : "");
			#endif
        #else /*CFG_OLD_TEMPLATE*/
			
			#ifdef CFGBTN_TYPE_SELECTION
			
				char html_template_header[] = "<!DOCTYPE html><meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\"><style>body{font-size:14px;font-family:HelveticaNeue,\"Helvetica Neue\",HelveticaNeueRoman,HelveticaNeue-Roman,\"Helvetica Neue Roman\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:400;font-stretch:normal;background:#00d151;color:#fff;line-height:20px;padding:0}.s{width:460px;margin:0 auto;margin-top:calc(50vh - 340px);border:solid 3px #fff;padding:0 10px 10px;border-radius:3px}#l{display:block;max-width:150px;height:155px;margin:-80px auto 20px;background:#00d151;padding-right:5px}#l path{fill:#000}.w{margin:3px 0 16px;padding:5px 0;border-radius:3px;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3)}h1,h3{margin:10px 8px;font-family:HelveticaNeueLight,HelveticaNeue-Light,\"Helvetica Neue Light\",HelveticaNeue,\"Helvetica Neue\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:300;font-stretch:normal;color:#000;font-size:23px}h1{margin-bottom:14px;color:#fff}span{display:block;margin:10px 7px 14px}i{display:block;font-style:normal;position:relative;border-bottom:solid 1px #00d151;height:42px}i:last-child{border:none}label{position:absolute;display:inline-block;top:0;left:8px;color:#00d151;line-height:41px;pointer-events:none}input,select{width:calc(100% - 145px);border:none;font-size:16px;line-height:40px;border-radius:0;letter-spacing:-.5px;background:#fff;color:#000;padding-left:144px;-webkit-appearance:none;-moz-appearance:none;appearance:none;outline:0!important;height:40px}select{padding:0;float:right;margin:1px 3px 1px 2px}button{width:100%;border:0;background:#000;padding:5px 10px;font-size:16px;line-height:40px;color:#fff;border-radius:3px;box-shadow:0 1px 3px rgba(0,0,0,.3);cursor:pointer}.c{background:#ffe836;position:fixed;width:100%;line-height:80px;color:#000;top:0;left:0;box-shadow:0 1px 3px rgba(0,0,0,.3);text-align:center;font-size:26px;z-index:100}@media all and (max-height:920px){.s{margin-top:80px}}@media all and (max-width:900px){.s{width:calc(100% - 20px);margin-top:40px;border:none;padding:0 8px;border-radius:0}#l{max-width:80px;height:auto;margin:10px auto 20px}h1,h3{font-size:19px}i{border:none;height:auto}label{display:block;margin:4px 0 12px;color:#00d151;font-size:13px;position:relative;line-height:18px}input,select{width:calc(100% - 10px);font-size:16px;line-height:28px;padding:0 5px;border-bottom:solid 1px #00d151}select{width:100%;float:none;margin:0}}</style><script type=\"text/javascript\">setTimeout(function(){var element =  document.getElementById('msg');if ( element != null ) element.style.visibility = \"hidden\";},3200);</script>";
				char html_template[] = "%s%s<div class=\"s\"><svg version=\"1.1\" id=\"l\" x=\"0\" y=\"0\" viewBox=\"0 0 200 200\" xml:space=\"preserve\"><path d=\"M59.3,2.5c18.1,0.6,31.8,8,40.2,23.5c3.1,5.7,4.3,11.9,4.1,18.3c-0.1,3.6-0.7,7.1-1.9,10.6c-0.2,0.7-0.1,1.1,0.6,1.5c12.8,7.7,25.5,15.4,38.3,23c2.9,1.7,5.8,3.4,8.7,5.3c1,0.6,1.6,0.6,2.5-0.1c4.5-3.6,9.8-5.3,15.7-5.4c12.5-0.1,22.9,7.9,25.2,19c1.9,9.2-2.9,19.2-11.8,23.9c-8.4,4.5-16.9,4.5-25.5,0.2c-0.7-0.3-1-0.2-1.5,0.3c-4.8,4.9-9.7,9.8-14.5,14.6c-5.3,5.3-10.6,10.7-15.9,16c-1.8,1.8-3.6,3.7-5.4,5.4c-0.7,0.6-0.6,1,0,1.6c3.6,3.4,5.8,7.5,6.2,12.2c0.7,7.7-2.2,14-8.8,18.5c-12.3,8.6-30.3,3.5-35-10.4c-2.8-8.4,0.6-17.7,8.6-22.8c0.9-0.6,1.1-1,0.8-2c-2-6.2-4.4-12.4-6.6-18.6c-6.3-17.6-12.7-35.1-19-52.7c-0.2-0.7-0.5-1-1.4-0.9c-12.5,0.7-23.6-2.6-33-10.4c-8-6.6-12.9-15-14.2-25c-1.5-11.5,1.7-21.9,9.6-30.7C32.5,8.9,42.2,4.2,53.7,2.7c0.7-0.1,1.5-0.2,2.2-0.2C57,2.4,58.2,2.5,59.3,2.5z M76.5,81c0,0.1,0.1,0.3,0.1,0.6c1.6,6.3,3.2,12.6,4.7,18.9c4.5,17.7,8.9,35.5,13.3,53.2c0.2,0.9,0.6,1.1,1.6,0.9c5.4-1.2,10.7-0.8,15.7,1.6c0.8,0.4,1.2,0.3,1.7-0.4c11.2-12.9,22.5-25.7,33.4-38.7c0.5-0.6,0.4-1,0-1.6c-5.6-7.9-6.1-16.1-1.3-24.5c0.5-0.8,0.3-1.1-0.5-1.6c-9.1-4.7-18.1-9.3-27.2-14c-6.8-3.5-13.5-7-20.3-10.5c-0.7-0.4-1.1-0.3-1.6,0.4c-1.3,1.8-2.7,3.5-4.3,5.1c-4.2,4.2-9.1,7.4-14.7,9.7C76.9,80.3,76.4,80.3,76.5,81z M89,42.6c0.1-2.5-0.4-5.4-1.5-8.1C83,23.1,74.2,16.9,61.7,15.8c-10-0.9-18.6,2.4-25.3,9.7c-8.4,9-9.3,22.4-2.2,32.4c6.8,9.6,19.1,14.2,31.4,11.9C79.2,67.1,89,55.9,89,42.6z M102.1,188.6c0.6,0.1,1.5-0.1,2.4-0.2c9.5-1.4,15.3-10.9,11.6-19.2c-2.6-5.9-9.4-9.6-16.8-8.6c-8.3,1.2-14.1,8.9-12.4,16.6C88.2,183.9,94.4,188.6,102.1,188.6z M167.7,88.5c-1,0-2.1,0.1-3.1,0.3c-9,1.7-14.2,10.6-10.8,18.6c2.9,6.8,11.4,10.3,19,7.8c7.1-2.3,11.1-9.1,9.6-15.9C180.9,93,174.8,88.5,167.7,88.5z\"/></svg><h1>%s</h1><span>LAST STATE: %s<br>Firmware: %s<br>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X</span><form method=\"post\"><div class=\"w\"><h3>Wi-Fi Settings</h3><i><input name=\"sid\" value=\"%s\"><label>Network name</label></i><i><input name=\"wpw\"><label>Password</label></i></div><div class=\"w\"><h3>Supla Settings</h3><i><input name=\"svr\" value=\"%s\"><label>Server</label></i><i><input type=\"number\" name=\"lid\" value=\"%i\"><label>Location ID</label></i><i><input name=\"pwd\"><label>Location Password</label></i></div><div class=\"w\"><h3>Additional Settings</h3><i><select name=\"cfg\"><option value=\"0\" %s>button<option value=\"1\" %s>switch</select><label>Button type</label></i></div><button type=\"submit\">SAVE</button></form></div><br><br>";
			
				
				bufflen = strlen(supla_esp_devconn_laststate())
							  +strlen(dev_name)
							  +strlen(SUPLA_ESP_SOFTVER)
							  +strlen(supla_esp_cfg.WIFI_SSID)
							  +strlen(supla_esp_cfg.Server)
							  +strlen(html_template_header)
							  +strlen(html_template)
							  +200;
				
				buffer = (char*)os_malloc(bufflen);	
				
				ets_snprintf(buffer,
						bufflen,
						html_template,
						html_template_header,
						data_saved == 1 ? "<div id=\"msg\" class=\"c\">Data saved</div>" : "",
						dev_name,
						supla_esp_devconn_laststate(),
						SUPLA_ESP_SOFTVER,
						(unsigned char)supla_esp_cfg.GUID[0],
						(unsigned char)supla_esp_cfg.GUID[1],
						(unsigned char)supla_esp_cfg.GUID[2],
						(unsigned char)supla_esp_cfg.GUID[3],
						(unsigned char)supla_esp_cfg.GUID[4],
						(unsigned char)supla_esp_cfg.GUID[5],
						(unsigned char)supla_esp_cfg.GUID[6],
						(unsigned char)supla_esp_cfg.GUID[7],
						(unsigned char)supla_esp_cfg.GUID[8],
						(unsigned char)supla_esp_cfg.GUID[9],
						(unsigned char)supla_esp_cfg.GUID[10],
						(unsigned char)supla_esp_cfg.GUID[11],
						(unsigned char)supla_esp_cfg.GUID[12],
						(unsigned char)supla_esp_cfg.GUID[13],
						(unsigned char)supla_esp_cfg.GUID[14],
						(unsigned char)supla_esp_cfg.GUID[15],
						(unsigned char)mac[0],
						(unsigned char)mac[1],
						(unsigned char)mac[2],
						(unsigned char)mac[3],
						(unsigned char)mac[4],
						(unsigned char)mac[5],
						supla_esp_cfg.WIFI_SSID,
						supla_esp_cfg.Server,
						supla_esp_cfg.LocationID,
						supla_esp_cfg.CfgButtonType == BTN_TYPE_BUTTON ? "selected" : "",
						supla_esp_cfg.CfgButtonType == BTN_TYPE_SWITCH ? "selected" : "");
			
			
			#elif defined(BTN1_2_TYPE_SELECTION)
			
				
				char html_template_header[] = "<!DOCTYPE html><meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\"><style>body{font-size:14px;font-family:HelveticaNeue,\"Helvetica Neue\",HelveticaNeueRoman,HelveticaNeue-Roman,\"Helvetica Neue Roman\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:400;font-stretch:normal;background:#00d151;color:#fff;line-height:20px;padding:0}.s{width:460px;margin:0 auto;margin-top:calc(50vh - 340px);border:solid 3px #fff;padding:0 10px 10px;border-radius:3px}#l{display:block;max-width:150px;height:155px;margin:-80px auto 20px;background:#00d151;padding-right:5px}#l path{fill:#000}.w{margin:3px 0 16px;padding:5px 0;border-radius:3px;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3)}h1,h3{margin:10px 8px;font-family:HelveticaNeueLight,HelveticaNeue-Light,\"Helvetica Neue Light\",HelveticaNeue,\"Helvetica Neue\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:300;font-stretch:normal;color:#000;font-size:23px}h1{margin-bottom:14px;color:#fff}span{display:block;margin:10px 7px 14px}i{display:block;font-style:normal;position:relative;border-bottom:solid 1px #00d151;height:42px}i:last-child{border:none}label{position:absolute;display:inline-block;top:0;left:8px;color:#00d151;line-height:41px;pointer-events:none}input,select{width:calc(100% - 145px);border:none;font-size:16px;line-height:40px;border-radius:0;letter-spacing:-.5px;background:#fff;color:#000;padding-left:144px;-webkit-appearance:none;-moz-appearance:none;appearance:none;outline:0!important;height:40px}select{padding:0;float:right;margin:1px 3px 1px 2px}button{width:100%;border:0;background:#000;padding:5px 10px;font-size:16px;line-height:40px;color:#fff;border-radius:3px;box-shadow:0 1px 3px rgba(0,0,0,.3);cursor:pointer}.c{background:#ffe836;position:fixed;width:100%;line-height:80px;color:#000;top:0;left:0;box-shadow:0 1px 3px rgba(0,0,0,.3);text-align:center;font-size:26px;z-index:100}@media all and (max-height:920px){.s{margin-top:80px}}@media all and (max-width:900px){.s{width:calc(100% - 20px);margin-top:40px;border:none;padding:0 8px;border-radius:0}#l{max-width:80px;height:auto;margin:10px auto 20px}h1,h3{font-size:19px}i{border:none;height:auto}label{display:block;margin:4px 0 12px;color:#00d151;font-size:13px;position:relative;line-height:18px}input,select{width:calc(100% - 10px);font-size:16px;line-height:28px;padding:0 5px;border-bottom:solid 1px #00d151}select{width:100%;float:none;margin:0}}</style><script type=\"text/javascript\">setTimeout(function(){var element =  document.getElementById('msg');if ( element != null ) element.style.visibility = \"hidden\";},3200);</script>";
				char html_template[] = "%s%s<div class=\"s\"><svg version=\"1.1\" id=\"l\" x=\"0\" y=\"0\" viewBox=\"0 0 200 200\" xml:space=\"preserve\"><path d=\"M59.3,2.5c18.1,0.6,31.8,8,40.2,23.5c3.1,5.7,4.3,11.9,4.1,18.3c-0.1,3.6-0.7,7.1-1.9,10.6c-0.2,0.7-0.1,1.1,0.6,1.5c12.8,7.7,25.5,15.4,38.3,23c2.9,1.7,5.8,3.4,8.7,5.3c1,0.6,1.6,0.6,2.5-0.1c4.5-3.6,9.8-5.3,15.7-5.4c12.5-0.1,22.9,7.9,25.2,19c1.9,9.2-2.9,19.2-11.8,23.9c-8.4,4.5-16.9,4.5-25.5,0.2c-0.7-0.3-1-0.2-1.5,0.3c-4.8,4.9-9.7,9.8-14.5,14.6c-5.3,5.3-10.6,10.7-15.9,16c-1.8,1.8-3.6,3.7-5.4,5.4c-0.7,0.6-0.6,1,0,1.6c3.6,3.4,5.8,7.5,6.2,12.2c0.7,7.7-2.2,14-8.8,18.5c-12.3,8.6-30.3,3.5-35-10.4c-2.8-8.4,0.6-17.7,8.6-22.8c0.9-0.6,1.1-1,0.8-2c-2-6.2-4.4-12.4-6.6-18.6c-6.3-17.6-12.7-35.1-19-52.7c-0.2-0.7-0.5-1-1.4-0.9c-12.5,0.7-23.6-2.6-33-10.4c-8-6.6-12.9-15-14.2-25c-1.5-11.5,1.7-21.9,9.6-30.7C32.5,8.9,42.2,4.2,53.7,2.7c0.7-0.1,1.5-0.2,2.2-0.2C57,2.4,58.2,2.5,59.3,2.5z M76.5,81c0,0.1,0.1,0.3,0.1,0.6c1.6,6.3,3.2,12.6,4.7,18.9c4.5,17.7,8.9,35.5,13.3,53.2c0.2,0.9,0.6,1.1,1.6,0.9c5.4-1.2,10.7-0.8,15.7,1.6c0.8,0.4,1.2,0.3,1.7-0.4c11.2-12.9,22.5-25.7,33.4-38.7c0.5-0.6,0.4-1,0-1.6c-5.6-7.9-6.1-16.1-1.3-24.5c0.5-0.8,0.3-1.1-0.5-1.6c-9.1-4.7-18.1-9.3-27.2-14c-6.8-3.5-13.5-7-20.3-10.5c-0.7-0.4-1.1-0.3-1.6,0.4c-1.3,1.8-2.7,3.5-4.3,5.1c-4.2,4.2-9.1,7.4-14.7,9.7C76.9,80.3,76.4,80.3,76.5,81z M89,42.6c0.1-2.5-0.4-5.4-1.5-8.1C83,23.1,74.2,16.9,61.7,15.8c-10-0.9-18.6,2.4-25.3,9.7c-8.4,9-9.3,22.4-2.2,32.4c6.8,9.6,19.1,14.2,31.4,11.9C79.2,67.1,89,55.9,89,42.6z M102.1,188.6c0.6,0.1,1.5-0.1,2.4-0.2c9.5-1.4,15.3-10.9,11.6-19.2c-2.6-5.9-9.4-9.6-16.8-8.6c-8.3,1.2-14.1,8.9-12.4,16.6C88.2,183.9,94.4,188.6,102.1,188.6z M167.7,88.5c-1,0-2.1,0.1-3.1,0.3c-9,1.7-14.2,10.6-10.8,18.6c2.9,6.8,11.4,10.3,19,7.8c7.1-2.3,11.1-9.1,9.6-15.9C180.9,93,174.8,88.5,167.7,88.5z\"/></svg><h1>%s</h1><span>LAST STATE: %s<br>Firmware: %s<br>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X</span><form method=\"post\"><div class=\"w\"><h3>Wi-Fi Settings</h3><i><input name=\"sid\" value=\"%s\"><label>Network name</label></i><i><input name=\"wpw\"><label>Password</label></i></div><div class=\"w\"><h3>Supla Settings</h3><i><input name=\"svr\" value=\"%s\"><label>Server</label></i><i><input type=\"number\" name=\"lid\" value=\"%i\"><label>Location ID</label></i><i><input name=\"pwd\"><label>Location Password</label></i></div><div class=\"w\"><h3>Additional Settings</h3><i><select name=\"bt1\"><option value=\"0\" %s>button<option value=\"1\" %s>switch</select><label>Input1 type:</label></i><i><select name=\"bt2\"><option value=\"0\" %s>button<option value=\"1\" %s>switch</select><label>Input2 type:</label></i></div><button type=\"submit\">SAVE</button></form></div><br><br>";
			
				
				bufflen = strlen(supla_esp_devconn_laststate())
							  +strlen(dev_name)
							  +strlen(SUPLA_ESP_SOFTVER)
							  +strlen(supla_esp_cfg.WIFI_SSID)
							  +strlen(supla_esp_cfg.Server)
							  +strlen(html_template_header)
							  +strlen(html_template)
							  +200;
				
				buffer = (char*)os_malloc(bufflen);	
				
				ets_snprintf(buffer,
						bufflen,
						html_template,
						html_template_header,
						data_saved == 1 ? "<div id=\"msg\" class=\"c\">Data saved</div>" : "",
						dev_name,
						supla_esp_devconn_laststate(),
						SUPLA_ESP_SOFTVER,
						(unsigned char)supla_esp_cfg.GUID[0],
						(unsigned char)supla_esp_cfg.GUID[1],
						(unsigned char)supla_esp_cfg.GUID[2],
						(unsigned char)supla_esp_cfg.GUID[3],
						(unsigned char)supla_esp_cfg.GUID[4],
						(unsigned char)supla_esp_cfg.GUID[5],
						(unsigned char)supla_esp_cfg.GUID[6],
						(unsigned char)supla_esp_cfg.GUID[7],
						(unsigned char)supla_esp_cfg.GUID[8],
						(unsigned char)supla_esp_cfg.GUID[9],
						(unsigned char)supla_esp_cfg.GUID[10],
						(unsigned char)supla_esp_cfg.GUID[11],
						(unsigned char)supla_esp_cfg.GUID[12],
						(unsigned char)supla_esp_cfg.GUID[13],
						(unsigned char)supla_esp_cfg.GUID[14],
						(unsigned char)supla_esp_cfg.GUID[15],
						(unsigned char)mac[0],
						(unsigned char)mac[1],
						(unsigned char)mac[2],
						(unsigned char)mac[3],
						(unsigned char)mac[4],
						(unsigned char)mac[5],
						supla_esp_cfg.WIFI_SSID,
						supla_esp_cfg.Server,
						supla_esp_cfg.LocationID,
						supla_esp_cfg.Button1Type == BTN_TYPE_BUTTON ? "selected" : "",
						supla_esp_cfg.Button1Type == BTN_TYPE_SWITCH ? "selected" : "",
						supla_esp_cfg.Button2Type == BTN_TYPE_BUTTON ? "selected" : "",
						supla_esp_cfg.Button2Type == BTN_TYPE_SWITCH ? "selected" : "");
				
			
			#else
			
				char html_template_header[] = "<!DOCTYPE html><meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\"><style>body{font-size:14px;font-family:HelveticaNeue,\"Helvetica Neue\",HelveticaNeueRoman,HelveticaNeue-Roman,\"Helvetica Neue Roman\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:400;font-stretch:normal;background:#00d151;color:#fff;line-height:20px;padding:0}.s{width:460px;margin:0 auto;margin-top:calc(50vh - 340px);border:solid 3px #fff;padding:0 10px 10px;border-radius:3px}#l{display:block;max-width:150px;height:155px;margin:-80px auto 20px;background:#00d151;padding-right:5px}#l path{fill:#000}.w{margin:3px 0 16px;padding:5px 0;border-radius:3px;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3)}h1,h3{margin:10px 8px;font-family:HelveticaNeueLight,HelveticaNeue-Light,\"Helvetica Neue Light\",HelveticaNeue,\"Helvetica Neue\",TeXGyreHerosRegular,Helvetica,Tahoma,Geneva,Arial,sans-serif;font-weight:300;font-stretch:normal;color:#000;font-size:23px}h1{margin-bottom:14px;color:#fff}span{display:block;margin:10px 7px 14px}i{display:block;font-style:normal;position:relative;border-bottom:solid 1px #00d151;height:42px}i:last-child{border:none}label{position:absolute;display:inline-block;top:0;left:8px;color:#00d151;line-height:41px;pointer-events:none}input,select{width:calc(100% - 145px);border:none;font-size:16px;line-height:40px;border-radius:0;letter-spacing:-.5px;background:#fff;color:#000;padding-left:144px;-webkit-appearance:none;-moz-appearance:none;appearance:none;outline:0!important;height:40px}select{padding:0;float:right;margin:1px 3px 1px 2px}button{width:100%;border:0;background:#000;padding:5px 10px;font-size:16px;line-height:40px;color:#fff;border-radius:3px;box-shadow:0 1px 3px rgba(0,0,0,.3);cursor:pointer}.c{background:#ffe836;position:fixed;width:100%;line-height:80px;color:#000;top:0;left:0;box-shadow:0 1px 3px rgba(0,0,0,.3);text-align:center;font-size:26px;z-index:100}@media all and (max-height:920px){.s{margin-top:80px}}@media all and (max-width:900px){.s{width:calc(100% - 20px);margin-top:40px;border:none;padding:0 8px;border-radius:0}#l{max-width:80px;height:auto;margin:10px auto 20px}h1,h3{font-size:19px}i{border:none;height:auto}label{display:block;margin:4px 0 12px;color:#00d151;font-size:13px;position:relative;line-height:18px}input,select{width:calc(100% - 10px);font-size:16px;line-height:28px;padding:0 5px;border-bottom:solid 1px #00d151}select{width:100%;float:none;margin:0}}</style><script type=\"text/javascript\">setTimeout(function(){var element =  document.getElementById('msg');if ( element != null ) element.style.visibility = \"hidden\";},3200);</script>";
				char html_template[] = "%s%s<div class=\"s\"><svg version=\"1.1\" id=\"l\" x=\"0\" y=\"0\" viewBox=\"0 0 200 200\" xml:space=\"preserve\"><path d=\"M59.3,2.5c18.1,0.6,31.8,8,40.2,23.5c3.1,5.7,4.3,11.9,4.1,18.3c-0.1,3.6-0.7,7.1-1.9,10.6c-0.2,0.7-0.1,1.1,0.6,1.5c12.8,7.7,25.5,15.4,38.3,23c2.9,1.7,5.8,3.4,8.7,5.3c1,0.6,1.6,0.6,2.5-0.1c4.5-3.6,9.8-5.3,15.7-5.4c12.5-0.1,22.9,7.9,25.2,19c1.9,9.2-2.9,19.2-11.8,23.9c-8.4,4.5-16.9,4.5-25.5,0.2c-0.7-0.3-1-0.2-1.5,0.3c-4.8,4.9-9.7,9.8-14.5,14.6c-5.3,5.3-10.6,10.7-15.9,16c-1.8,1.8-3.6,3.7-5.4,5.4c-0.7,0.6-0.6,1,0,1.6c3.6,3.4,5.8,7.5,6.2,12.2c0.7,7.7-2.2,14-8.8,18.5c-12.3,8.6-30.3,3.5-35-10.4c-2.8-8.4,0.6-17.7,8.6-22.8c0.9-0.6,1.1-1,0.8-2c-2-6.2-4.4-12.4-6.6-18.6c-6.3-17.6-12.7-35.1-19-52.7c-0.2-0.7-0.5-1-1.4-0.9c-12.5,0.7-23.6-2.6-33-10.4c-8-6.6-12.9-15-14.2-25c-1.5-11.5,1.7-21.9,9.6-30.7C32.5,8.9,42.2,4.2,53.7,2.7c0.7-0.1,1.5-0.2,2.2-0.2C57,2.4,58.2,2.5,59.3,2.5z M76.5,81c0,0.1,0.1,0.3,0.1,0.6c1.6,6.3,3.2,12.6,4.7,18.9c4.5,17.7,8.9,35.5,13.3,53.2c0.2,0.9,0.6,1.1,1.6,0.9c5.4-1.2,10.7-0.8,15.7,1.6c0.8,0.4,1.2,0.3,1.7-0.4c11.2-12.9,22.5-25.7,33.4-38.7c0.5-0.6,0.4-1,0-1.6c-5.6-7.9-6.1-16.1-1.3-24.5c0.5-0.8,0.3-1.1-0.5-1.6c-9.1-4.7-18.1-9.3-27.2-14c-6.8-3.5-13.5-7-20.3-10.5c-0.7-0.4-1.1-0.3-1.6,0.4c-1.3,1.8-2.7,3.5-4.3,5.1c-4.2,4.2-9.1,7.4-14.7,9.7C76.9,80.3,76.4,80.3,76.5,81z M89,42.6c0.1-2.5-0.4-5.4-1.5-8.1C83,23.1,74.2,16.9,61.7,15.8c-10-0.9-18.6,2.4-25.3,9.7c-8.4,9-9.3,22.4-2.2,32.4c6.8,9.6,19.1,14.2,31.4,11.9C79.2,67.1,89,55.9,89,42.6z M102.1,188.6c0.6,0.1,1.5-0.1,2.4-0.2c9.5-1.4,15.3-10.9,11.6-19.2c-2.6-5.9-9.4-9.6-16.8-8.6c-8.3,1.2-14.1,8.9-12.4,16.6C88.2,183.9,94.4,188.6,102.1,188.6z M167.7,88.5c-1,0-2.1,0.1-3.1,0.3c-9,1.7-14.2,10.6-10.8,18.6c2.9,6.8,11.4,10.3,19,7.8c7.1-2.3,11.1-9.1,9.6-15.9C180.9,93,174.8,88.5,167.7,88.5z\"/></svg><h1>%s</h1><span>LAST STATE: %s<br>Firmware: %s<br>GUID: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X</span><form method=\"post\"><div class=\"w\"><h3>Wi-Fi Settings</h3><i><input name=\"sid\" value=\"%s\"><label>Network name</label></i><i><input name=\"wpw\"><label>Password</label></i></div><div class=\"w\"><h3>Supla Settings</h3><i><input name=\"svr\" value=\"%s\"><label>Server</label></i><i><input type=\"number\" name=\"lid\" value=\"%i\"><label>Location ID</label></i><i><input name=\"pwd\"><label>Location Password</label></i></div><button type=\"submit\">SAVE</button></form></div><br><br>";
				
				int bufflen = strlen(supla_esp_devconn_laststate())
							  +strlen(dev_name)
							  +strlen(SUPLA_ESP_SOFTVER)
							  +strlen(supla_esp_cfg.WIFI_SSID)
							  +strlen(supla_esp_cfg.Server)
							  +strlen(html_template_header)
							  +strlen(html_template)
							  +200;
				
				buffer = (char*)os_malloc(bufflen);	
				
				ets_snprintf(buffer,
						bufflen,
						html_template,
						html_template_header,
						data_saved == 1 ? "<div id=\"msg\" class=\"c\">Data saved</div>" : "",
						dev_name,
						supla_esp_devconn_laststate(),
						SUPLA_ESP_SOFTVER,
						(unsigned char)supla_esp_cfg.GUID[0],
						(unsigned char)supla_esp_cfg.GUID[1],
						(unsigned char)supla_esp_cfg.GUID[2],
						(unsigned char)supla_esp_cfg.GUID[3],
						(unsigned char)supla_esp_cfg.GUID[4],
						(unsigned char)supla_esp_cfg.GUID[5],
						(unsigned char)supla_esp_cfg.GUID[6],
						(unsigned char)supla_esp_cfg.GUID[7],
						(unsigned char)supla_esp_cfg.GUID[8],
						(unsigned char)supla_esp_cfg.GUID[9],
						(unsigned char)supla_esp_cfg.GUID[10],
						(unsigned char)supla_esp_cfg.GUID[11],
						(unsigned char)supla_esp_cfg.GUID[12],
						(unsigned char)supla_esp_cfg.GUID[13],
						(unsigned char)supla_esp_cfg.GUID[14],
						(unsigned char)supla_esp_cfg.GUID[15],
						(unsigned char)mac[0],
						(unsigned char)mac[1],
						(unsigned char)mac[2],
						(unsigned char)mac[3],
						(unsigned char)mac[4],
						(unsigned char)mac[5],
						supla_esp_cfg.WIFI_SSID,
						supla_esp_cfg.Server,
						supla_esp_cfg.LocationID);
				
			#endif
			
        #endif

	#endif
	
	if ( buffer ) {
		supla_esp_http_ok((struct espconn *)arg, buffer);
		os_free(buffer);
	}
	
	
}


void ICACHE_FLASH_ATTR
supla_esp_discon_callback(void *arg) {
	
    struct espconn *conn = (struct espconn *)arg;
	
    if ( conn->reverse != NULL ) {
    	os_free(conn->reverse);
    	conn->reverse = NULL;
    }
}

void ICACHE_FLASH_ATTR
supla_esp_connectcb(void *arg)
{
    struct espconn *conn = (struct espconn *)arg;
    
    TrivialHttpParserVars *pVars = os_malloc(sizeof(TrivialHttpParserVars));
    memset(pVars, 0, sizeof(TrivialHttpParserVars));
    conn->reverse = pVars;

    espconn_regist_recvcb(conn, supla_esp_recv_callback );
    espconn_regist_disconcb(conn, supla_esp_discon_callback);
}


void ICACHE_FLASH_ATTR
supla_esp_cfgmode_start(void) {
	
	char APSSID[] = AP_SSID;
	char mac[6];

	#ifdef BOARD_BEFORE_CFGMODE_START
	supla_esp_board_before_cfgmode_start();
	#endif

	supla_esp_devconn_before_cfgmode_start();

	wifi_get_macaddr(SOFTAP_IF, mac);

	struct softap_config apconfig;
	struct espconn *conn;
	
	if ( supla_esp_cfgmode_entertime != 0 )
		return;
	
	supla_esp_devconn_stop();

	supla_esp_cfgmode_entertime = system_get_time();
	
	supla_log(LOG_DEBUG, "ENTER CFG MODE");
	supla_esp_gpio_state_cfgmode();

	int apssid_len = strlen(APSSID);

	if ( apssid_len+14 > 32 )
		apssid_len = 18;

	memcpy(apconfig.ssid, APSSID, apssid_len);

	ets_snprintf(&apconfig.ssid[apssid_len],
			14,
 			"-%02X%02X%02X%02X%02X%02X",
			(unsigned char)mac[0],
			(unsigned char)mac[1],
			(unsigned char)mac[2],
			(unsigned char)mac[3],
			(unsigned char)mac[4],
			(unsigned char)mac[5]);

	apconfig.password[0] = 0;
	apconfig.ssid_len = apssid_len+13;
	apconfig.channel = 1;
	apconfig.authmode = AUTH_OPEN;
	apconfig.ssid_hidden = 0;
	apconfig.max_connection = 4;
	apconfig.beacon_interval = 100;
	
	wifi_set_opmode(SOFTAP_MODE);
	wifi_softap_set_config(&apconfig);

	conn = (struct espconn *)os_malloc(sizeof(struct espconn));
	memset( conn, 0, sizeof( struct espconn ) );

	espconn_create(conn);
	espconn_regist_time(conn, 5, 0);
	
	conn->type = ESPCONN_TCP;
	conn->state = ESPCONN_NONE;

	conn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
	conn->proto.tcp->local_port = 80;

	espconn_regist_connectcb(conn, supla_esp_connectcb);
	espconn_accept(conn);
	

}

char ICACHE_FLASH_ATTR
supla_esp_cfgmode_started(void) {

	return supla_esp_cfgmode_entertime == 0 ? 0 : 1;
}

