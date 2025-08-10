#pragma once
#include <lvgl.h>

// Proveer callbacks para navegación
typedef void (*ui_simple_cb)();

void ui_config_set_callbacks(ui_simple_cb on_back_to_main,
                             ui_simple_cb on_open_wifi,
                             ui_simple_cb on_open_add_sensor);

// Crea la pantalla de Configuración y la carga.
void lv_create_config_menu();