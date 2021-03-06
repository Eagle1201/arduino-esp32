/*
 ESP8266WiFiGeneric.cpp - WiFi library for esp8266

 Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
 This file is part of the esp8266 core for Arduino environment.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

 Reworked on 28 Dec 2015 by Markus Sattler

 */

#include "WiFi.h"
#include "WiFiGeneric.h"

extern "C" {
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_event_loop.h>
#include <lwip/ip_addr.h>

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/dns.h"

#include "esp32-hal-log.h"
}

//#include "WiFiClient.h"
//#include "WiFiUdp.h"

#undef min
#undef max
#include <vector>

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Generic WiFi function -----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

// arduino dont like std::vectors move static here
static std::vector<WiFiEventCbList_t> cbEventList;

bool WiFiGenericClass::_persistent = true;
wifi_mode_t WiFiGenericClass::_forceSleepLastMode = WIFI_MODE_NULL;

WiFiGenericClass::WiFiGenericClass()
{

}

/**
 * set callback function
 * @param cbEvent WiFiEventCb
 * @param event optional filter (WIFI_EVENT_MAX is all events)
 */
void WiFiGenericClass::onEvent(WiFiEventCb cbEvent, system_event_id_t event)
{
    if(!cbEvent) {
        return;
    }
    WiFiEventCbList_t newEventHandler;
    newEventHandler.cb = cbEvent;
    newEventHandler.event = event;
    cbEventList.push_back(newEventHandler);
}

/**
 * removes a callback form event handler
 * @param cbEvent WiFiEventCb
 * @param event optional filter (WIFI_EVENT_MAX is all events)
 */
void WiFiGenericClass::removeEvent(WiFiEventCb cbEvent, system_event_id_t event)
{
    if(!cbEvent) {
        return;
    }

    for(uint32_t i = 0; i < cbEventList.size(); i++) {
        WiFiEventCbList_t entry = cbEventList[i];
        if(entry.cb == cbEvent && entry.event == event) {
            cbEventList.erase(cbEventList.begin() + i);
        }
    }
}

/**
 * callback for WiFi events
 * @param arg
 */
esp_err_t WiFiGenericClass::_eventCallback(void *arg, system_event_t *event)
{
    log_d("wifi evt: %d", event->event_id);

    if(event->event_id == SYSTEM_EVENT_SCAN_DONE) {
        WiFiScanClass::_scanDone();
    } else if(event->event_id == SYSTEM_EVENT_STA_DISCONNECTED) {
        uint8_t reason = event->event_info.disconnected.reason;
        if(reason == WIFI_REASON_NO_AP_FOUND) {
            WiFiSTAClass::_setStatus(WL_NO_SSID_AVAIL);
        } else if(reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_ASSOC_FAIL) {
            WiFiSTAClass::_setStatus(WL_CONNECT_FAILED);
        } else if(reason == WIFI_REASON_BEACON_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            WiFiSTAClass::_setStatus(WL_CONNECTION_LOST);
        } else {
            WiFiSTAClass::_setStatus(WL_DISCONNECTED);
        }
        log_d("wifi reason: %d", reason);
    } else if(event->event_id == SYSTEM_EVENT_STA_START) {
        WiFiSTAClass::_setStatus(WL_DISCONNECTED);
    } else if(event->event_id == SYSTEM_EVENT_STA_STOP) {
        WiFiSTAClass::_setStatus(WL_NO_SHIELD);
    } else if(event->event_id == SYSTEM_EVENT_STA_GOT_IP) {
        WiFiSTAClass::_setStatus(WL_CONNECTED);
    }

    for(uint32_t i = 0; i < cbEventList.size(); i++) {
        WiFiEventCbList_t entry = cbEventList[i];
        if(entry.cb) {
            if(entry.event == (system_event_id_t) event->event_id || entry.event == SYSTEM_EVENT_MAX) {
                entry.cb((system_event_id_t) event->event_id);
            }
        }
    }
    return ESP_OK;
}

/**
 * Return the current channel associated with the network
 * @return channel (1-13)
 */
int32_t WiFiGenericClass::channel(void)
{
    uint8_t primaryChan;
    wifi_second_chan_t secondChan;
    esp_wifi_get_channel(&primaryChan, &secondChan);
    return primaryChan;
}


/**
 * store WiFi config in SDK flash area
 * @param persistent
 */
void WiFiGenericClass::persistent(bool persistent)
{
    _persistent = persistent;
}


/**
 * set new mode
 * @param m WiFiMode_t
 */
bool WiFiGenericClass::mode(wifi_mode_t m)
{
    if(getMode() == m) {
        return true;
    }
    return esp_wifi_set_mode(m) == ESP_OK;
}

/**
 * get WiFi mode
 * @return WiFiMode
 */
wifi_mode_t WiFiGenericClass::getMode()
{
    uint8_t mode;
    esp_wifi_get_mode((wifi_mode_t*)&mode);
    return (wifi_mode_t)mode;
}

/**
 * control STA mode
 * @param enable bool
 * @return ok
 */
bool WiFiGenericClass::enableSTA(bool enable)
{

    wifi_mode_t currentMode = getMode();
    bool isEnabled = ((currentMode & WIFI_MODE_STA) != 0);

    if(isEnabled != enable) {
        if(enable) {
            return mode((wifi_mode_t)(currentMode | WIFI_MODE_STA));
        } else {
            return mode((wifi_mode_t)(currentMode & (~WIFI_MODE_STA)));
        }
    } else {
        return true;
    }
}

/**
 * control AP mode
 * @param enable bool
 * @return ok
 */
bool WiFiGenericClass::enableAP(bool enable)
{

    wifi_mode_t currentMode = getMode();
    bool isEnabled = ((currentMode & WIFI_MODE_AP) != 0);

    if(isEnabled != enable) {
        if(enable) {
            return mode((wifi_mode_t)(currentMode | WIFI_MODE_AP));
        } else {
            return mode((wifi_mode_t)(currentMode & (~WIFI_MODE_AP)));
        }
    } else {
        return true;
    }
}


// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ Generic Network function ---------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void wifi_dns_found_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg);

/**
 * Resolve the given hostname to an IP address.
 * @param aHostname     Name to be resolved
 * @param aResult       IPAddress structure to store the returned IP address
 * @return 1 if aIPAddrString was successfully converted to an IP address,
 *          else error code
 */
static bool _dns_busy = false;

int WiFiGenericClass::hostByName(const char* aHostname, IPAddress& aResult)
{
    ip_addr_t addr;
    aResult = static_cast<uint32_t>(0);
    err_t err = dns_gethostbyname(aHostname, &addr, &wifi_dns_found_callback, &aResult);
    _dns_busy = err == ERR_INPROGRESS;
    while(_dns_busy);
    if(err == ERR_INPROGRESS && aResult) {
        //found by search
    } else if(err == ERR_OK && addr.u_addr.ip4.addr) {
        aResult = addr.u_addr.ip4.addr;
    } else {
        return 0;
    }
    return 1;
}

/**
 * DNS callback
 * @param name
 * @param ipaddr
 * @param callback_arg
 */
void wifi_dns_found_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    if(ipaddr) {
        (*reinterpret_cast<IPAddress*>(callback_arg)) = ipaddr->u_addr.ip4.addr;
    }
    _dns_busy = false;
}

/**
 * Boot and start WiFi
 * This method get's called on boot if you use any of the WiFi methods.
 * If you do not link to this library, WiFi will not be started.
 * */
#include "nvs_flash.h"

void initWiFi()
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    nvs_flash_init();
    system_init();
    tcpip_adapter_init();
    esp_event_loop_init(WiFiGenericClass::_eventCallback, NULL);
    esp_wifi_init(&cfg);
}

void startWiFi()
{
    esp_err_t err;
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool auto_connect = false;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        log_e("esp_wifi_start fail %d\n", err);
        return;
    }

    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        log_e("esp_wifi_get_mode fail %d\n", err);
        return;
    }

    err = esp_wifi_get_auto_connect(&auto_connect);
    if ((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && auto_connect) {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            log_e("esp_wifi_connect fail %d\n", err);
        }
    }
}

