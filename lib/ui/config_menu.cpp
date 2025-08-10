#include "config_menu.h"
#include <aqs_images.h>   
static ui_simple_cb cb_back_main = nullptr;
static ui_simple_cb cb_open_wifi = nullptr;
static ui_simple_cb cb_open_add_sensor = nullptr;

void ui_config_set_callbacks(ui_simple_cb a, ui_simple_cb b, ui_simple_cb c) {
  cb_back_main     = a;
  cb_open_wifi     = b;
  cb_open_add_sensor = c;
}

void lv_create_config_menu() {
  LV_IMAGE_DECLARE(image_back);

  static lv_style_t style_btn_text;
  static bool style_btn_text_inited = false;
  if(!style_btn_text_inited) {
    style_btn_text_inited = true;
    lv_style_init(&style_btn_text);
    lv_style_set_text_font(&style_btn_text, &lv_font_montserrat_28);
  }

  lv_obj_t * scr = lv_obj_create(NULL);
  lv_obj_set_size(scr, lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
  lv_scr_load(scr);

  lv_obj_t * title = lv_label_create(scr);
  lv_label_set_text(title, "Menu de configuracion");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t * btn_back = lv_image_create(scr);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, [](lv_event_t * e) {
    if (cb_back_main) cb_back_main();
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t * list = lv_obj_create(scr);
  lv_obj_set_size(list, lv_pct(90), lv_pct(65));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 12, 0);
  lv_obj_set_style_pad_all(list, 10, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_shadow_width(list, 0, 0);
  lv_obj_set_style_outline_width(list, 0, 0);

  // Wi‑Fi
  lv_obj_t * btn_wifi = lv_btn_create(list);
  lv_obj_set_width(btn_wifi, lv_pct(100));
  lv_obj_set_height(btn_wifi, 50);
  lv_obj_t * lbl_wifi = lv_label_create(btn_wifi);
  lv_label_set_text(lbl_wifi, "WiFi");
  lv_obj_center(lbl_wifi);
  lv_obj_add_style(lbl_wifi, &style_btn_text, 0);
  lv_obj_add_event_cb(btn_wifi, [](lv_event_t * e) {
    if (cb_open_wifi) cb_open_wifi();
  }, LV_EVENT_CLICKED, NULL);

  // Agregar sensor
  lv_obj_t * btn_sensor = lv_btn_create(list);
  lv_obj_set_width(btn_sensor, lv_pct(100));
  lv_obj_set_height(btn_sensor, 50);
  lv_obj_t * lbl_sensor = lv_label_create(btn_sensor);
  lv_label_set_text(lbl_sensor, "Agregar sensor");
  lv_obj_center(lbl_sensor);
  lv_obj_add_style(lbl_sensor, &style_btn_text, 0);
  lv_obj_add_event_cb(btn_sensor, [](lv_event_t * e) {
    if (cb_open_add_sensor) cb_open_add_sensor();
  }, LV_EVENT_CLICKED, NULL);
}