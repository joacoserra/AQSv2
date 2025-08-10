#include "wifi_async.h"
#include <WiFi.h>
#include <Arduino.h>

typedef void (*wifi_ssid_clicked_cb)(const char*);
struct ScanCtx {
  lv_obj_t* list;
  int max_items;
  wifi_ssid_clicked_cb on_click;
  uint8_t attempts;
};

void connect_to_wifi(const char* ssid, const char* password, void (*on_connected)(void), void (*on_failed)(void)) {
  WiFi.disconnect(true);
  WiFi.begin(ssid, password);

  // Overlay mínimo "Conectando..."
  lv_obj_clean(lv_screen_active());
  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text(label, "Conectando...");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  struct ConnectCtx { lv_obj_t * label; uint16_t tries; void (*ok)(); void (*fail)(); };
  auto *ctx = new ConnectCtx{ label, 0, on_connected, on_failed };

  lv_timer_t * t = lv_timer_create([](lv_timer_t * t){
    auto * ctx = (ConnectCtx*)lv_timer_get_user_data(t);
    wl_status_t st = WiFi.status();

    if (st == WL_CONNECTED) {
      lv_label_set_text(ctx->label, "¡Conectado!");
      lv_timer_del(t);

      // pequeña pausa sin bloquear y luego callback
      lv_timer_t * back = lv_timer_create_basic();
      lv_timer_set_period(back, 700);
      lv_timer_set_repeat_count(back, 1);
      lv_timer_set_cb(back, [](lv_timer_t * tb){
        auto * ctx = (ConnectCtx*)lv_timer_get_user_data(tb);
        if (ctx->ok) ctx->ok();
        delete ctx;
      });
      lv_timer_set_user_data(back, ctx);
      return;
    }

    if (++ctx->tries >= 50) { // ~10 s @200ms
      lv_label_set_text(ctx->label, "Error al conectar");
      lv_timer_del(t);

      lv_timer_t * back = lv_timer_create_basic();
      lv_timer_set_period(back, 1000);
      lv_timer_set_repeat_count(back, 1);
      lv_timer_set_cb(back, [](lv_timer_t * tb){
        auto * ctx = (ConnectCtx*)lv_timer_get_user_data(tb);
        if (ctx->fail) ctx->fail();
        delete ctx;
      });
      lv_timer_set_user_data(back, ctx);
    }
  }, 200, ctx);
  lv_timer_set_user_data(t, ctx);
}

static void ssid_btn_clicked_cb(lv_event_t * e) {
  lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t * label = lv_obj_get_child(btn, 0);
  const char * ssid_selected = lv_label_get_text(label);
  ScanCtx* ctx = (ScanCtx*)lv_event_get_user_data(e);
  if (ctx && ctx->on_click) ctx->on_click(ssid_selected);
}

static void start_scan_task(void *arg){
  // corre en core 0, no bloquea la UI
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);   // async
  vTaskDelete(NULL);
}

static void start_scan_async(ScanCtx* ctx) {
  xTaskCreatePinnedToCore(start_scan_task, "wifi_scan_start",
                          4096, ctx, 1, NULL, 0); // core 0
}

#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING   (-1)
#endif
#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED    (-2)
#endif

static void poll_scan_timer_cb(lv_timer_t * t) {
  ScanCtx* ctx = (ScanCtx*)lv_timer_get_user_data(t);
  int8_t r = WiFi.scanComplete();   // -1 running, -2 failed, >=0 count
  if (r == WIFI_SCAN_RUNNING) return;

  if (r == WIFI_SCAN_FAILED) {
    if (++ctx->attempts <= 3) {     // reintenta hasta 3 veces
      start_scan_async(ctx);
      return;
    }
    lv_timer_del(t);
    lv_obj_clean(ctx->list);
    lv_obj_t * lbl = lv_label_create(ctx->list);
    lv_label_set_text(lbl, "Error al escanear WiFi.");
    lv_obj_center(lbl);
    delete ctx;
    return;
  }

  lv_timer_del(t);
  lv_obj_clean(ctx->list);

  // lista de SSIDs
  int count = (ctx->max_items > 0) ? LV_MIN((int)r, ctx->max_items) : r;
  for (int i=0;i<count;i++){
    String ssid = WiFi.SSID(i);

    lv_obj_t * btn = lv_btn_create(ctx->list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 36);

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, ssid.c_str());
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, ssid_btn_clicked_cb, LV_EVENT_CLICKED, ctx);
  }

  if (count == 0) {
    lv_obj_t * lbl = lv_label_create(ctx->list);
    lv_label_set_text(lbl, "No se encontraron redes.");
    lv_obj_center(lbl);
  }
}

void wifi_scan_async(lv_obj_t* list_container, int max_items,
                     wifi_ssid_clicked_cb on_ssid_clicked) {
  lv_obj_clean(list_container);
  lv_obj_t * scanning = lv_label_create(list_container);
  lv_label_set_text(scanning, "Escaneando...");
  lv_obj_center(scanning);

  ScanCtx* ctx = new ScanCtx{list_container, max_items, on_ssid_clicked, 0};
  start_scan_async(ctx);

  lv_timer_t * poll = lv_timer_create(poll_scan_timer_cb, 150, ctx);
  lv_timer_set_user_data(poll, ctx);
}