#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Iniciar task de touch en core 0.
// 'ts' es un puntero opaco al XPT2046_Touchscreen (en C++ se castea).
void touch_start_task(void* ts, int screenW, int screenH);

// read_cb para LVGL (no bloqueante)
void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

// Hook opcional: se llama en cada PRESS; devolver !=0 si hizo algo.
typedef int (*touch_hook_t)(void);
extern touch_hook_t touch_on_any_press;

#ifdef __cplusplus
}
#endif