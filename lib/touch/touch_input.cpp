#include "touch_input.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <XPT2046_Touchscreen.h>

struct TouchSample { int16_t x, y; bool pressed; uint32_t ts; };
static QueueHandle_t q = nullptr;
static XPT2046_Touchscreen* g_ts = nullptr;
static int W=0, H=0;

touch_hook_t touch_on_any_press = nullptr;

static void touch_task(void*){
  for(;;){
    TouchSample s{};
    bool pressed = g_ts && (g_ts->tirqTouched() || g_ts->touched());
    if (pressed) {
      TS_Point p = g_ts->getPoint();
      // Tus coeficientes:
      float ax=0.001f, bx=-0.130f, dx=498.426f;
      float ay=-0.087f, by=0.001f, dy=339.434f;
      int x = (int)(ay*p.x + by*p.y + dy);
      int y = (int)(ax*p.x + bx*p.y + dx);
      if (x<0) x=0; if (x>W-1) x=W-1;
      if (y<0) y=0; if (y>H-1) y=H-1;
      s = { (int16_t)x, (int16_t)y, true, (uint32_t)millis() };
    } else {
      s = { 0, 0, false, (uint32_t)millis() };
    }
    if (q) xQueueOverwrite(q, &s);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void touch_start_task(void* ts, int screenW, int screenH){
  g_ts = reinterpret_cast<XPT2046_Touchscreen*>(ts);
  W=screenW; H=screenH;
  q = xQueueCreate(1, sizeof(TouchSample));
  xTaskCreatePinnedToCore(touch_task, "touch_task", 4096, nullptr,
                          configMAX_PRIORITIES-2, nullptr, 0); // core 0
}

void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data){
  (void)indev;
  TouchSample s{};
  if (!q || xQueuePeek(q, &s, 0) != pdTRUE) { data->state = LV_INDEV_STATE_RELEASED; return; }
  data->state   = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = s.x; data->point.y = s.y;
  if (s.pressed && touch_on_any_press) (void)touch_on_any_press();
}