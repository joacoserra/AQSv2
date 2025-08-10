#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Crea la UI principal sobre la pantalla activa (no hace lv_scr_load)
void lv_create_main_gui(void);

// Pantalla simple de "Agregar sensor" con botón para volver a Config
void open_sensor_list_screen(void);

#ifdef __cplusplus
}
#endif