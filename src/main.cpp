#include <lvgl.h>
#include <TFT_eSPI.h>
#include <aqs_images.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include <esp_now.h>

// === Módulos nuevos ===
#include <touch_input.h>
#include <config_menu.h>
#include <wifi_menu.h>
#include <dashboard.h>

// ---------- Tu modelo/datos/sensores (se quedan aquí) ----------
struct DashboardPage {
  lv_obj_t * cont;
  lv_obj_t * name;

  // valores
  lv_obj_t * temp;
  lv_obj_t * hum;
  lv_obj_t * ppm;

  // íconos junto a los valores
  lv_obj_t * icon_temp;
  lv_obj_t * icon_hum;
  lv_obj_t * icon_mono;
  lv_obj_t * icon_state;     // clean/alert al lado de ppm

  // botón/imagen settings (abajo-izq)
  lv_obj_t * btn_settings;

  int  sensorIdx;
  bool alert_active;
  bool alert_blink;
};

static std::vector<DashboardPage> pages; // máx 2
static int current_page = 0;
static lv_obj_t * btn_prev = nullptr;
static lv_obj_t * btn_next = nullptr;

typedef struct struct_message {
  char  sensor;
  float temp;
  float float_hum;
  float mono;
  bool  ackRequired;
  bool  isAck;
} struct_message;

struct DiscoveredSensor {
  uint8_t mac[6];
  String  macStr;
  String  name;
  struct_message last;
  uint32_t lastSeen;
};

std::vector<DiscoveredSensor> sensors;
int selectedSensorIndex = -1;
static const uint32_t SENSOR_STALE_MS = 30000;

// ---------- UI globals que ya tenías ----------
//static lv_obj_t * text_label_time_location = nullptr;
//static lv_timer_t * ui_timer = nullptr;
static bool alert_active = false;
static bool buzzer_muted = false;

#define BUZZER_PIN 15
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 480
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

SPIClass touchscreenSPI(HSPI);
#define XPT2046_IRQ 27
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK 14
#define XPT2046_CS 33
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

String location = "Bahia Blanca";
const char degree_symbol[] = "\u00B0C"; // ajustá si usás F

// Forward UI (tu dashboard)
static DashboardPage create_sensor_page(lv_obj_t * parent, int sensorIdx);
static void show_page(int i);
static void update_page_from_sensor(DashboardPage &p);
static void update_nav_arrows(lv_obj_t * parent);
static lv_style_t st_title, st_value, st_unit;
static bool styles_inited = false;

// ESP-NOW
static void on_espnow_recv(const uint8_t *mac, const uint8_t *incomingData, int len);

// Helpers
static String get_formatted_datetime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00/00/0000 00:00";
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M", &timeinfo);
  return String(buffer);
}

// Logger LVGL sin flush
static void log_print(lv_log_level_t level, const char * buf) {
  if (level >= LV_LOG_LEVEL_ERROR) Serial.println(buf);
}

// Navegación simple
static lv_obj_t * main_screen = nullptr;
static void go_to_main() { lv_scr_load(main_screen); }
static void open_config_menu() { lv_create_config_menu(); }
static void open_wifi_menu() { lv_create_wifi_menu(); }
// open_sensor_list_screen() – si lo tenés en este archivo, poné aquí su forward:
//static void open_sensor_list_screen();

// Setup
void setup() {
  Serial.begin(115200);

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inicializando ESP-NOW");
  } else {
    esp_now_register_recv_cb(on_espnow_recv);
  }

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  configTzTime("GMT+3", "pool.ntp.org", "time.nist.gov");

  // LVGL
  lv_init();
  lv_log_register_print_cb(log_print);

  // Touch HW
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  // Display
  lv_display_t * disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

  // Touch task (core 0) + input device
  touch_start_task((void*)&touchscreen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);

  // Hook de mute al tocar:
  touch_on_any_press = []() -> int {
    if (alert_active) { buzzer_muted = true; digitalWrite(BUZZER_PIN, LOW); }
    return 1;
  };

  // Callbacks de UI (módulos)
  ui_config_set_callbacks(
    /*on_back_to_main*/ [](){ go_to_main(); },
    /*on_open_wifi*/    [](){ lv_create_wifi_menu(); },
    /*on_open_add_sensor*/ [](){ open_sensor_list_screen(); }
  );
  ui_wifi_set_callbacks(
    /*on_back_to_config*/ [](){ lv_create_config_menu(); },
    /*on_connected_ok*/   [](){ go_to_main(); }
  );

  // Splash muy simple → luego dashboard
  lv_obj_t * splash = lv_obj_create(NULL);
  lv_scr_load(splash);
  lv_obj_t * label = lv_label_create(splash);
  lv_label_set_text(label, "Air Quality System");
  lv_obj_center(label);

  lv_timer_t * once = lv_timer_create_basic();
  lv_timer_set_period(once, 900);
  lv_timer_set_repeat_count(once, 1);
  lv_timer_set_cb(once, [](lv_timer_t *){
    lv_obj_clean(lv_screen_active());
    lv_create_main_gui();
    main_screen = lv_screen_active();
  });
}

void loop() {
  static uint32_t last = millis();
  lv_timer_handler();               // v9
  uint32_t now = millis();
  lv_tick_inc(now - last);          // delta real
  last = now;
  delay(5);
}

// ---------------------- TU DASHBOARD (igual que antes) ----------------------
// (Copia tus funciones: create_sensor_page, show_page, update_page_from_sensor,
//  update_nav_arrows, open_sensor_list_screen, etc. No hace falta tocarlas)

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

// ===== Helpers =====
static void nav_prev_cb(lv_event_t*) { if (pages.empty()) return; int i = (current_page - 1 + (int)pages.size()) % (int)pages.size(); lv_obj_clear_flag(pages[i].cont, LV_OBJ_FLAG_HIDDEN); show_page(i); }
static void nav_next_cb(lv_event_t*) { if (pages.empty()) return; int i = (current_page + 1) % (int)pages.size(); lv_obj_clear_flag(pages[i].cont, LV_OBJ_FLAG_HIDDEN); show_page(i); }

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

static void update_page_from_sensor(DashboardPage &p) {
  // si el índice es inválido, mostrar placeholders
  if (p.sensorIdx < 0 || p.sensorIdx >= (int)sensors.size()) {
    lv_label_set_text(p.temp, "--.-");
    lv_label_set_text(p.hum,  "--");
    lv_label_set_text(p.ppm,  "---");
    lv_image_set_src(p.icon_state, &image_cleanair);
    return;
  }

  const auto & s = sensors[p.sensorIdx].last;

  // valores (texto)
  char bufT[16], bufH[16], bufP[16];
  snprintf(bufT, sizeof(bufT), "%.1f", s.temp);
  snprintf(bufH, sizeof(bufH), "%.0f", s.float_hum);
  snprintf(bufP, sizeof(bufP), "%.0f", s.mono);

  lv_label_set_text(p.temp, bufT);
  lv_label_set_text(p.hum,  bufH);
  lv_label_set_text(p.ppm,  bufP);

  // estado de aire en base a monóxido
  bool is_alert = (s.mono >= 100.0f);
  p.alert_active = is_alert;

  if (is_alert) {
    lv_image_set_src(p.icon_state, &image_alert);
    // beep si no está silenciado
    static bool buzzFlip=false;
    if (!buzzer_muted) {
      buzzFlip = !buzzFlip;
      digitalWrite(BUZZER_PIN, buzzFlip ? HIGH : LOW);
    } else {
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    lv_image_set_src(p.icon_state, &image_cleanair);
    digitalWrite(BUZZER_PIN, LOW);
  }
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

static DashboardPage create_sensor_page(lv_obj_t * parent, int sensorIdx) {
  ensure_styles();

  DashboardPage p{};
  p.sensorIdx = sensorIdx;
  p.alert_active = false;
  p.alert_blink  = false;

  // Contenedor principal de la página
  p.cont = lv_obj_create(parent);
  lv_obj_set_size(p.cont, lv_pct(90), lv_pct(70));
  lv_obj_align(p.cont, LV_ALIGN_CENTER, 0, 15);
  lv_obj_set_style_bg_opa(p.cont, LV_OPA_0, 0);
  lv_obj_set_style_border_width(p.cont, 0, 0);
  lv_obj_set_style_shadow_width(p.cont, 0, 0);

  // Nombre del sensor (arriba)
  p.name = lv_label_create(p.cont);
  const char* title = (sensorIdx >= 0 && sensorIdx < (int)sensors.size() && sensors[sensorIdx].name.length())
                        ? sensors[sensorIdx].name.c_str()
                        : "Sensor";
  lv_label_set_text(p.name, title);
  lv_obj_add_style(p.name, &st_title, 0);
  lv_obj_align(p.name, LV_ALIGN_TOP_MID, 0, 0);

  // ===== Temperatura =====
  p.temp = lv_label_create(p.cont);
  lv_label_set_text(p.temp, "--.-");
  lv_obj_add_style(p.temp, &st_value, 0);
  lv_obj_align(p.temp, LV_ALIGN_TOP_LEFT, 80, 60);

  lv_obj_t * unitT = lv_label_create(p.cont);
  lv_label_set_text(unitT, "°C");
  lv_obj_add_style(unitT, &st_unit, 0);
  lv_obj_align_to(unitT, p.temp, LV_ALIGN_OUT_RIGHT_TOP, 6, 6);

  p.icon_temp = lv_image_create(p.cont);
  lv_image_set_src(p.icon_temp, &image_weather_temperature);
  lv_obj_align_to(p.icon_temp, p.temp, LV_ALIGN_OUT_LEFT_MID, -12, 0);

  // ===== Humedad =====
  p.hum = lv_label_create(p.cont);
  lv_label_set_text(p.hum, "--");
  lv_obj_add_style(p.hum, &st_value, 0);
  lv_obj_align(p.hum, LV_ALIGN_TOP_LEFT, 80, 120);

  lv_obj_t * unitH = lv_label_create(p.cont);
  lv_label_set_text(unitH, "%");
  lv_obj_add_style(unitH, &st_unit, 0);
  lv_obj_align_to(unitH, p.hum, LV_ALIGN_OUT_RIGHT_TOP, 6, 6);

  p.icon_hum = lv_image_create(p.cont);
  lv_image_set_src(p.icon_hum, &image_weather_humidity);
  lv_obj_align_to(p.icon_hum, p.hum, LV_ALIGN_OUT_LEFT_MID, -12, 0);

  // ===== Monóxido =====
  p.ppm = lv_label_create(p.cont);
  lv_label_set_text(p.ppm, "---");
  lv_obj_add_style(p.ppm, &st_value, 0);
  lv_obj_align(p.ppm, LV_ALIGN_TOP_LEFT, 80, 180);

  lv_obj_t * unitP = lv_label_create(p.cont);
  lv_label_set_text(unitP, "ppm");
  lv_obj_add_style(unitP, &st_unit, 0);
  lv_obj_align_to(unitP, p.ppm, LV_ALIGN_OUT_RIGHT_TOP, 6, 6);

  p.icon_mono = lv_image_create(p.cont);
  lv_image_set_src(p.icon_mono, &image_monoxide);
  lv_obj_align_to(p.icon_mono, p.ppm, LV_ALIGN_OUT_LEFT_MID, -12, 0);

  p.icon_state = lv_image_create(p.cont);
  lv_image_set_src(p.icon_state, &image_cleanair);
  lv_obj_align_to(p.icon_state, p.ppm, LV_ALIGN_OUT_RIGHT_MID, 24, 0);

  // ===== Settings (abajo-izquierda de la pantalla) =====
  p.btn_settings = lv_image_create(parent); // en la pantalla (no dentro del contenedor)
  lv_image_set_src(p.btn_settings, &image_settings);
  lv_obj_align(p.btn_settings, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_obj_add_flag(p.btn_settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(p.btn_settings, 8);
  lv_obj_add_event_cb(p.btn_settings, [](lv_event_t *){
    lv_create_config_menu();
  }, LV_EVENT_CLICKED, NULL);

  // Arrancamos oculta, la muestra show_page() si no es la actual
  lv_obj_add_flag(p.cont, LV_OBJ_FLAG_HIDDEN);

  return p;
}
// ---------------------- ESP-NOW RX ----------------------
static void on_espnow_recv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!mac || len != sizeof(struct_message)) return;
  // … tu lógica actual para registrar/actualizar sensores y ACK …
}