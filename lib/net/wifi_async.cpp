#include "wifi_async.h"
#include <WiFi.h>

struct ScanCtx {
  lv_obj_t* list;
  int max_items;
  wifi_ssid_clicked_cb on_click;
};

static void ssid_btn_clicked_cb(lv_event_t * e) {
  lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t * label = lv_obj_get_child(btn, 0);
  const char * ssid_selected = lv_label_get_text(label);
  ScanCtx* ctx = (ScanCtx*)lv_event_get_user_data(e);
  if (ctx && ctx->on_click) ctx->on_click(ssid_selected);
}

static void poll_scan_timer_cb(lv_timer_t * t) {
  ScanCtx* ctx = (ScanCtx*)lv_timer_get_user_data(t);
  int8_t r = WiFi.scanComplete();         // -1 running, -2 error, >=0 count
  if (r == WIFI_SCAN_RUNNING) return;

  lv_timer_del(t);
  lv_obj_clean(ctx->list);

  if (r < 0) {
    lv_obj_t * lbl = lv_label_create(ctx->list);
    lv_label_set_text(lbl, "Error al escanear WiFi.");
    lv_obj_center(lbl);
    delete ctx;
    return;
  }

  static lv_style_t style_wifi_text;
  static bool style_inited = false;
  if(!style_inited) {
    style_inited = true;
    lv_style_init(&style_wifi_text);
    lv_style_set_text_font(&style_wifi_text, &lv_font_montserrat_18);
  }

  int count = (ctx->max_items > 0) ? LV_MIN((int)r, ctx->max_items) : r;
  for (int i=0;i<count;i++){
    String ssid = WiFi.SSID(i);

    lv_obj_t * btn = lv_btn_create(ctx->list);
    lv_obj_set_width(btn, lv_pct(70));
    lv_obj_set_height(btn, 32);

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, ssid.c_str());
    lv_obj_center(label);
    lv_obj_add_style(label, &style_wifi_text, 0);

    lv_obj_add_event_cb(btn, ssid_btn_clicked_cb, LV_EVENT_CLICKED, ctx);
  }

  lv_obj_update_layout(ctx->list);
  lv_obj_scroll_to_y(ctx->list, 0, LV_ANIM_OFF);

  // NOTA: no liberamos ctx aquí para poder usarlo en el callback de click.
}

void wifi_scan_async(lv_obj_t* list_container, int max_items,
                     wifi_ssid_clicked_cb on_ssid_clicked) {
  lv_obj_clean(list_container);
  lv_obj_t * scanning = lv_label_create(list_container);
  lv_label_set_text(scanning, "Escaneando...");
  lv_obj_center(scanning);

  WiFi.scanDelete();
  WiFi.scanNetworks(true); // async

  ScanCtx* ctx = new ScanCtx{list_container, max_items, on_ssid_clicked};
  lv_timer_t * poll = lv_timer_create(poll_scan_timer_cb, 120, ctx);
  lv_timer_set_user_data(poll, ctx);
}

// -------- Conectar async ----------
struct ConnCtx {
  lv_obj_t* lbl;
  uint16_t tries;
  wifi_simple_cb ok;
  wifi_simple_cb fail;
};

static void poll_conn_timer_cb(lv_timer_t * t) {
  ConnCtx* ctx = (ConnCtx*)lv_timer_get_user_data(t);
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    lv_label_set_text(ctx->lbl, "¡Conectado!");
    lv_timer_del(t);
    if (ctx->ok) ctx->ok();
    delete ctx;
    return;
  }
  if (++ctx->tries >= 50) { // ~10 s @200ms
    lv_label_set_text(ctx->lbl, "Error al conectar");
    lv_timer_del(t);
    if (ctx->fail) ctx->fail();
    delete ctx;
    return;
  }
}

void wifi_connect_async(const char* ssid, const char* pass,
                        lv_obj_t* status_label,
                        wifi_simple_cb on_connected,
                        wifi_simple_cb on_failed) {
  WiFi.disconnect(true);
  WiFi.begin(ssid, pass);
  lv_label_set_text(status_label, "Conectando...");
  ConnCtx* ctx = new ConnCtx{status_label, 0, on_connected, on_failed};
  lv_timer_t * t = lv_timer_create(poll_conn_timer_cb, 200, ctx);
  lv_timer_set_user_data(t, ctx);
}