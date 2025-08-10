#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_simple_cb)(void);
typedef void (*wifi_ssid_clicked_cb)(const char* ssid);

// Escaneo NO bloqueante: llena list_container y llama on_ssid_clicked(ssid) al tocar.
void wifi_scan_async(lv_obj_t* list_container, int max_items,
                     wifi_ssid_clicked_cb on_ssid_clicked);

// Conexión NO bloqueante: actualiza status_label y llama a los callbacks.
void wifi_connect_async(const char* ssid, const char* pass,
                        lv_obj_t* status_label,
                        wifi_simple_cb on_connected,
                        wifi_simple_cb on_failed);

#ifdef __cplusplus
}
#endif