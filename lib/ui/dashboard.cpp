#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <aqs_images.h>
#include "dashboard.h"
#include "config_menu.h"   // lv_create_config_menu()

// ===== Tus imágenes (de aqs_images.h) como "extern" con LV_IMAGE_DECLARE =====
LV_IMAGE_DECLARE(image_weather_temperature);
LV_IMAGE_DECLARE(image_weather_humidity);
LV_IMAGE_DECLARE(image_monoxide);
LV_IMAGE_DECLARE(image_cleanair);
LV_IMAGE_DECLARE(image_alert);
LV_IMAGE_DECLARE(image_settings);
LV_IMAGE_DECLARE(image_back);

// ===== Estado que ya existe en main.cpp (solo lo referenciamos) =====
struct struct_message {
  char  sensor;
  float temp;
  float float_hum;
  float mono;
  bool  ackRequired;
  bool  isAck;
};

struct DiscoveredSensor {
  uint8_t mac[6];
  String  macStr;
  String  name;
  struct_message last;
  uint32_t lastSeen;
};

extern std::vector<DiscoveredSensor> sensors;   // definido en main.cpp
extern int selectedSensorIndex;                 // definido en main.cpp
extern String location;                         // definido en main.cpp

// ===== UI local del dashboard =====
namespace {

struct DashboardPage {
  lv_obj_t * cont;
  lv_obj_t * name;

  // valores
  lv_obj_t * temp;
  lv_obj_t * hum;
  lv_obj_t * ppm;

  // íconos
  lv_obj_t * icon_temp;
  lv_obj_t * icon_hum;
  lv_obj_t * icon_mono;
  lv_obj_t * icon_state;     // clean/alert al lado de ppm

  // botón/imagen settings (abajo-izq)
  lv_obj_t * btn_settings;

  int  sensorIdx;
  bool alert_active;
};

static std::vector<DashboardPage> pages;
static int current_page = 0;

static lv_obj_t  *text_label_time_location = nullptr;
static lv_timer_t *ui_timer = nullptr;

static lv_style_t st_title, st_value, st_unit;
static bool styles_inited = false;

static void ensure_styles() {
  if (styles_inited) return;
  styles_inited = true;

  lv_style_init(&st_title);
  lv_style_set_text_font(&st_title, &lv_font_montserrat_22);

  lv_style_init(&st_value);
  lv_style_set_text_font(&st_value, &lv_font_montserrat_28);

  lv_style_init(&st_unit);
  lv_style_set_text_font(&st_unit, &lv_font_montserrat_18);
}

static String fmt_datetime() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "00/00/0000 00:00";
  char buf[30];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &ti);
  return String(buf);
}

static void show_page(int i);

static void nav_prev_cb(lv_event_t*) {
  if (pages.empty()) return;
  int i = (current_page - 1 + (int)pages.size()) % (int)pages.size();
  show_page(i);
}
static void nav_next_cb(lv_event_t*) {
  if (pages.empty()) return;
  int i = (current_page + 1) % (int)pages.size();
  show_page(i);
}

static void update_nav_arrows(lv_obj_t * parent) {
  static lv_obj_t * nav_left = nullptr, * nav_right = nullptr;
  bool show = (pages.size() > 1);

  if (!nav_left) {
    nav_left = lv_label_create(parent);
    lv_label_set_text(nav_left, LV_SYMBOL_LEFT);
    lv_obj_align(nav_left, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_flag(nav_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nav_left, nav_prev_cb, LV_EVENT_CLICKED, NULL);
  }
  if (!nav_right) {
    nav_right = lv_label_create(parent);
    lv_label_set_text(nav_right, LV_SYMBOL_RIGHT);
    lv_obj_align(nav_right, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_flag(nav_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(nav_right, nav_next_cb, LV_EVENT_CLICKED, NULL);
  }

  if (show) {
    lv_obj_clear_flag(nav_left,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(nav_right, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(nav_left,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(nav_right, LV_OBJ_FLAG_HIDDEN);
  }
}

static void settings_click_cb(lv_event_t *) {
  lv_create_config_menu();
}

static void update_page_from_sensor(DashboardPage &p) {
  // placeholders si no hay sensor
  if (p.sensorIdx < 0 || p.sensorIdx >= (int)sensors.size()) {
    lv_label_set_text(p.temp, "--.-");
    lv_label_set_text(p.hum,  "--");
    lv_label_set_text(p.ppm,  "---");
    lv_image_set_src(p.icon_state, &image_cleanair);
    p.alert_active = false;
    return;
  }

  const auto & s = sensors[p.sensorIdx].last;

  char bufT[32], bufH[32], bufP[32];
  snprintf(bufT, sizeof(bufT), "%.1f °C", s.temp);
  snprintf(bufH, sizeof(bufH), "%.0f %%", s.float_hum);
  snprintf(bufP, sizeof(bufP), "%.0f ppm", s.mono);

  lv_label_set_text(p.temp, bufT);
  lv_label_set_text(p.hum,  bufH);
  lv_label_set_text(p.ppm,  bufP);

  bool is_alert = (s.mono >= 100.0f);
  p.alert_active = is_alert;
  lv_image_set_src(p.icon_state, s.mono >= 100.0f ? &image_alert : &image_cleanair);
}

static DashboardPage create_sensor_page(lv_obj_t * parent, int sensorIdx) {
  ensure_styles();

  DashboardPage p{};
  p.sensorIdx    = sensorIdx;
  p.alert_active = false;

  // Contenedor
  p.cont = lv_obj_create(parent);
  lv_obj_set_size(p.cont, lv_pct(100), lv_pct(100));
  lv_obj_align(p.cont, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(p.cont, LV_OPA_0, 0);
  lv_obj_set_style_border_width(p.cont, 0, 0);
  lv_obj_set_style_shadow_width(p.cont, 0, 0);

  // Título: nombre del sensor si hay seleccionado
  const char* title = "Sensor";
  if (sensorIdx >= 0 && sensorIdx < (int)sensors.size() && sensors[sensorIdx].name.length()) {
    title = sensors[sensorIdx].name.c_str();
  }
  p.name = lv_label_create(p.cont);
  lv_label_set_text(p.name, title);
  lv_obj_add_style(p.name, &st_title, 0);
  lv_obj_align(p.name, LV_ALIGN_TOP_MID, 0, 0);

  // ===== Temperatura =====
  p.icon_temp = lv_image_create(p.cont);
  lv_image_set_src(p.icon_temp, &image_weather_temperature);
  lv_obj_align(p.icon_temp, LV_ALIGN_CENTER, 30, -60);

  p.temp = lv_label_create(p.cont);
  lv_label_set_text(p.temp, "--.- °C");
  lv_obj_align(p.temp, LV_ALIGN_CENTER, 95, -60);
  lv_obj_set_style_text_font(p.temp, &lv_font_montserrat_22, 0);

  // ===== Humedad =====
  p.icon_hum = lv_image_create(p.cont);
  lv_image_set_src(p.icon_hum, &image_weather_humidity);
  lv_obj_align(p.icon_hum, LV_ALIGN_CENTER, 30, 15);

  p.hum = lv_label_create(p.cont);
  lv_label_set_text(p.hum, "-- %");
  lv_obj_align(p.hum, LV_ALIGN_CENTER, 95, 15);
  lv_obj_set_style_text_font(p.hum, &lv_font_montserrat_22, 0);

  // ===== Monóxido =====
  p.icon_mono = lv_image_create(p.cont);
  lv_image_set_src(p.icon_mono, &image_monoxide);
  lv_obj_align(p.icon_mono, LV_ALIGN_CENTER, 30, 80);

  p.ppm = lv_label_create(p.cont);
  lv_label_set_text(p.ppm, "--- ppm");
  lv_obj_align(p.ppm, LV_ALIGN_CENTER, 120, 80);
  lv_obj_set_style_text_font(p.ppm, &lv_font_montserrat_22, 0);

  // Estado (limpio/alerta)
  p.icon_state = lv_image_create(p.cont);
  lv_image_set_src(p.icon_state, &image_cleanair);
  lv_obj_align(p.icon_state, LV_ALIGN_CENTER, -100, -10);

  // ===== Settings (abajo-izquierda, fijo en pantalla) =====
  p.btn_settings = lv_image_create(p.cont);
  lv_image_set_src(p.btn_settings, &image_settings);
  lv_obj_align(p.btn_settings, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_flag(p.btn_settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(p.btn_settings, settings_click_cb, LV_EVENT_CLICKED, NULL);

  // Arranca oculta; show_page() activa la que corresponda
  lv_obj_add_flag(p.cont, LV_OBJ_FLAG_HIDDEN);

  return p;
}

static void show_page(int i) {
  if (pages.empty()) return;
  if (i < 0) i = 0;
  if (i >= (int)pages.size()) i = (int)pages.size() - 1;

  for (int k=0; k<(int)pages.size(); ++k) {
    if (k == i) lv_obj_clear_flag(pages[k].cont, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag  (pages[k].cont, LV_OBJ_FLAG_HIDDEN);
  }
  current_page = i;
  update_nav_arrows(lv_screen_active());
}

} // namespace

// ============== API expuesta en dashboard.h ==============

void lv_create_main_gui(void) {
  // Texto fecha/hora + ubicación (arriba)
  text_label_time_location = lv_label_create(lv_screen_active());
  String line = fmt_datetime() + " | " + location;
  lv_label_set_text(text_label_time_location, line.c_str());
  lv_obj_align(text_label_time_location, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_text_font(text_label_time_location, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(text_label_time_location, lv_palette_main(LV_PALETTE_GREY), 0);

  // Páginas según sensores (o una vacía si no hay)
  pages.clear();
  if (sensors.empty()) {
    pages.push_back(create_sensor_page(lv_screen_active(), -1));
  } else {
    // Si tenés índice seleccionado, va primero
    int start = -1;
    if (selectedSensorIndex >= 0 && selectedSensorIndex < (int)sensors.size())
      start = selectedSensorIndex;

    if (start >= 0) pages.push_back(create_sensor_page(lv_screen_active(), start));

    for (int i=0; i<(int)sensors.size(); ++i) {
      if (i == start) continue;
      pages.push_back(create_sensor_page(lv_screen_active(), i));
    }
  }

  // Mostrar primera
  show_page(0);

  // Timer UI (500ms): hora y refresco de la página
  if (ui_timer) { lv_timer_del(ui_timer); ui_timer = nullptr; }
  ui_timer = lv_timer_create([](lv_timer_t *){
    // fecha/hora
    String dt = fmt_datetime();
    String l  = location + " - " + dt;
    lv_label_set_text(text_label_time_location, l.c_str());

    // actualizar lecturas de la página actual
    if (!pages.empty()) update_page_from_sensor(pages[current_page]);
  }, 500, NULL);
}

// ---------------- Pantalla "Agregar sensor" ----------------

static lv_obj_t * sensor_list_screen = nullptr;
static lv_obj_t * sensor_list_container = nullptr;
static lv_timer_t * sensor_scan_timer = nullptr;
static const uint32_t SENSOR_STALE_MS = 30000; // 30s de vigencia

static void sensor_back_btn_cb(lv_event_t *) {
  // Volver al menú de configuración (coincide con tu flujo actual)
  lv_create_config_menu();
}

static void populate_sensor_list();

static void sensor_btn_clicked_cb(lv_event_t * e) {
  size_t idx = (size_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
  selectedSensorIndex = (int)idx;

  // Volver al principal y reconstruir
  lv_obj_t * root = lv_obj_get_parent(sensor_list_screen);
  (void)root;
  lv_scr_load(lv_screen_active());
  lv_obj_clean(lv_screen_active());
  lv_create_main_gui();
}

static void sensor_scan_timer_cb(lv_timer_t *) {
  populate_sensor_list();
}

static void populate_sensor_list() {
  if (!sensor_list_container) return;

  lv_obj_clean(sensor_list_container);

  int shown = 0;
  uint32_t now = millis();

  for (size_t i = 0; i < sensors.size(); ++i) {
    if (now - sensors[i].lastSeen > SENSOR_STALE_MS) continue;

    lv_obj_t * btn = lv_btn_create(sensor_list_container);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 40);

    String line = (sensors[i].name.length() ? sensors[i].name : sensors[i].macStr);
    line += "   ";
    line += String(sensors[i].last.temp, 1) + "°C  ";
    line += String(sensors[i].last.float_hum, 0) + "%  ";
    line += String(sensors[i].last.mono, 0) + "ppm";

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, line.c_str());
    lv_obj_center(lbl);

    lv_obj_set_user_data(btn, (void*)i);
    lv_obj_add_event_cb(btn, sensor_btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    shown++;
  }

  if (shown == 0) {
    lv_obj_t * lbl = lv_label_create(sensor_list_container);
    lv_label_set_text(lbl, "No hay sensores detectados todavía.");
    lv_obj_center(lbl);
  }
}

void open_sensor_list_screen(void) {
  // cerrar anterior (si existía)
  if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
  if (sensor_list_screen) { lv_obj_del(sensor_list_screen); sensor_list_screen = nullptr; }

  sensor_list_screen = lv_obj_create(NULL);
  lv_scr_load(sensor_list_screen);

  // Título
  lv_obj_t * title = lv_label_create(sensor_list_screen);
  lv_label_set_text(title, "Sensores disponibles");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver (tu ícono)
  lv_obj_t * btn_back = lv_image_create(sensor_list_screen);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, sensor_back_btn_cb, LV_EVENT_CLICKED, NULL);

  // Lista scrollable
  sensor_list_container = lv_obj_create(sensor_list_screen);
  lv_obj_set_size(sensor_list_container, lv_pct(90), lv_pct(70));
  lv_obj_align(sensor_list_container, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(sensor_list_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(sensor_list_container, LV_DIR_VER);
  lv_obj_set_style_bg_opa(sensor_list_container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sensor_list_container, 0, 0);
  lv_obj_set_style_shadow_width(sensor_list_container, 0, 0);
  lv_obj_set_style_outline_width(sensor_list_container, 0, 0);
  lv_obj_set_style_pad_row(sensor_list_container, 8, 0);
  lv_obj_set_style_pad_all(sensor_list_container, 8, 0);

  // Poblar y arrancar refresco
  populate_sensor_list();
  sensor_scan_timer = lv_timer_create(sensor_scan_timer_cb, 2000, NULL);
}