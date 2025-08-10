#include "wifi_menu.h"
#include "../net/wifi_async.h"
#include "../keyboard/wifi_keyboard.h"
#include <aqs_images.h>

static ui_simple_cb cb_back_cfg = nullptr;
static ui_simple_cb cb_connected_ok = nullptr;

void ui_wifi_set_callbacks(ui_simple_cb a, ui_simple_cb b) {
  cb_back_cfg = a;
  cb_connected_ok = b;
}

static void show_wifi_keyboard(const char * ssid) {
  lv_obj_clean(lv_screen_active());

  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text_fmt(label, "%s", ssid);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t * ta = lv_textarea_create(lv_screen_active());
  lv_obj_set_width(ta, lv_pct(90));
  lv_obj_set_height(ta, 50);
  lv_textarea_set_password_mode(ta, true);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);

  lv_obj_t * kb = build_wifi_style_keyboard(lv_screen_active(), ta);

  // OK → conectar async
  lv_obj_add_event_cb(kb, [](lv_event_t * e) {
    auto kb = (lv_obj_t*)lv_event_get_target(e);
    auto ta = (lv_obj_t*)lv_keyboard_get_textarea(kb);
    const char* pass = lv_textarea_get_text(ta);

    lv_obj_t * status = lv_label_create(lv_screen_active());
    lv_label_set_text(status, "Conectando...");
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 0);

    const char* ssid = lv_label_get_text(lv_obj_get_child(lv_screen_active(), 0)); // el title
    wifi_connect_async(ssid, pass, status,
      [](){ // connected
        // pequeña pausa visual sin bloquear y volver a main
        lv_timer_t * back = lv_timer_create_basic();
        lv_timer_set_period(back, 700);
        lv_timer_set_repeat_count(back, 1);
        lv_timer_set_cb(back, [](lv_timer_t *){
          if (cb_connected_ok) cb_connected_ok();
        });
      },
      [](){ // failed
        lv_timer_t * back = lv_timer_create_basic();
        lv_timer_set_period(back, 1000);
        lv_timer_set_repeat_count(back, 1);
        lv_timer_set_cb(back, [](lv_timer_t *){
          if (cb_back_cfg) cb_back_cfg();
        });
      }
    );
  }, LV_EVENT_READY, NULL);

  // Back → volver a Wi‑Fi (lista)
  lv_obj_add_event_cb(kb, [](lv_event_t * e) {
    lv_create_wifi_menu();
  }, LV_EVENT_CANCEL, NULL);
}

void lv_create_wifi_menu() {
  LV_IMAGE_DECLARE(image_back);

  lv_obj_t * scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
  lv_scr_load(scr);

  lv_obj_t * title = lv_label_create(scr);
  lv_label_set_text(title, "Redes WiFi");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Volver
  lv_obj_t * btn_back = lv_image_create(scr);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, [](lv_event_t * e) {
    if (cb_back_cfg) cb_back_cfg();
  }, LV_EVENT_CLICKED, NULL);

  // Lista scrollable
  lv_obj_t * list = lv_obj_create(scr);
  lv_obj_set_size(list, lv_pct(90), lv_pct(70));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_style_pad_all(list, 8, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_anim_time(list, 0, 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_shadow_width(list, 0, 0);
  lv_obj_set_style_outline_width(list, 0, 0);

  lv_obj_set_flex_align(list,
    LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Escaneo NO bloqueante + callbacks
  wifi_scan_async(list, /*max_items*/10, [](const char* ssid){
    show_wifi_keyboard(ssid);
  });
}