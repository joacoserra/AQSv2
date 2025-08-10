#pragma once
#include <lvgl.h>

// Teclado reutilizable estilo Wi‑Fi con tecla Shift (↑).
// Devuelve el objeto teclado; usalo con un textarea pasado por parámetro.
lv_obj_t* build_wifi_style_keyboard(lv_obj_t* parent, lv_obj_t* textarea);