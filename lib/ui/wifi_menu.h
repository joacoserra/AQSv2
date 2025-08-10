#pragma once
#include <lvgl.h>

// Configurar callbacks de navegación
typedef void (*ui_simple_cb)();

void ui_wifi_set_callbacks(ui_simple_cb on_back_to_config, ui_simple_cb on_connected_ok);

// Abre la pantalla de “Redes Wi‑Fi”, hace scan async y permite conectar.
void lv_create_wifi_menu();