#pragma once
#include <lvgl.h>

using wifi_simple_cb = void (*)(void);
using wifi_ssid_clicked_cb = void (*)(const char*);

void wifi_scan_async(lv_obj_t* list_container, int max_items,
                     wifi_ssid_clicked_cb on_ssid_clicked);

void connect_to_wifi(const char* ssid, const char* password,
                     void (*on_connected)(void),
                     void (*on_failed)(void));