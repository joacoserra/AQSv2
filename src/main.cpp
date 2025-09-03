#include <lvgl.h>
#include <TFT_eSPI.h>
#include "aqs_images.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include <esp_now.h>
#include <DHT.h>
#include <MQ7.h>
#include <esp_wifi.h>

// ===== NEW: FreeRTOS ====
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =================== TUS GLOBALES ===================
static lv_display_t *g_disp = nullptr;
static lv_coord_t SW = 0, SH = 0;
static int g_curr_page = 0;

// Intervalos
#define UI_REFRESH_MS        300
#define LOCAL_READ_MS        4000

// ====== Paquete idéntico al EMISOR ======
typedef struct struct_message {
    char  sensor;       // 'A' o 'B'
    float temp;         // temperatura
    float float_hum;    // humedad
    float mono;         // CO ppm
    bool  ackRequired;  // si el emisor pide ACK
    bool  isAck;        // si este paquete es un ACK
} struct_message;

// ==== Descubrimiento de sensores ESP-NOW ====
struct DiscoveredSensor {
  uint8_t mac[6];
  String  macStr;     // "AA:BB:CC:DD:EE:FF"
  String  name;       // asignado por el usuario (p.ej. "Cocina")
  struct_message last;
  uint32_t lastSeen;  // millis() de último paquete
};

// ---- UI de "Sensores disponibles" ----
static lv_obj_t * sensor_list_screen = nullptr;
static lv_obj_t * sensor_list_container = nullptr;
static lv_timer_t * sensor_scan_timer = nullptr;

static lv_obj_t * pages = nullptr;

struct PageWidgets {
  lv_obj_t *name = nullptr;
  lv_obj_t *icon = nullptr;
  lv_obj_t *t   = nullptr;  // temperatura
  lv_obj_t *h   = nullptr;  // humedad
  lv_obj_t *co  = nullptr;  // ppm
};

static PageWidgets local_page;
static std::vector<PageWidgets> remote_pages;

// Pines sensores locales
#define DHTPIN  22 // Pin DHT11
#define DHTTYPE DHT11 // Tipo DHT11 o DHT22
DHT dht(DHTPIN, DHTTYPE);

#define MQ7_PIN 34
static float local_t = NAN, local_h = NAN, local_co = NAN;

// === Contexto para nombrar sensor ===
struct NameCtx { lv_obj_t * ta; size_t idx; };
static NameCtx g_name_ctx;

static std::vector<DiscoveredSensor> sensors;
static int selectedSensorIndex = -1;
static const uint32_t SENSOR_STALE_MS = 30000;

// Label nombre sensor activo
static lv_obj_t * text_label_sensor_name = nullptr;

// ====== Estado de recepción ======
static volatile bool espnow_has_data = false;
static uint32_t last_rx_ms = 0;
static const uint32_t ESPNOW_TIMEOUT_MS = 15000;

// Wi‑Fi
std::vector<String> availableSSIDs;
String wifi_ssid = "";
String wifi_password = "";
static const int WIFI_LIST_LIMIT = 10;

// Ubicación
String location = "Bahia Blanca";

// Almacenes de texto UI
String temperature;
String humidity;
String monoxide;

// Buzzer
#define BUZZER_PIN 15
static bool buzzer_muted = false;

// Unidades
#define TEMP_CELSIUS 1
#if TEMP_CELSIUS
  String temperature_unit = "";
  const char degree_symbol[] = "\u00B0C";
#else
  String temperature_unit = "&temperature_unit=fahrenheit";
  const char degree_symbol[] = "\u00B0F";
#endif

// ==== TOUCH XPT2046 (HSPI) ====
#define XPT2046_IRQ 27
#define XPT2046_MOSI 13
#define XPT2046_MISO 12
#define XPT2046_CLK 14
#define XPT2046_CS 33
SPIClass touchscreenSPI(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// Pantalla
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 480
#define LVGL_BUF_LINES 10
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / LVGL_BUF_LINES * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

// ======= PROTOTIPOS (los tuyos) =======
void get_weather_description(int code);
void get_weather_data();
void log_print(lv_log_level_t level, const char * buf);
void lv_create_main_gui(void);
String get_formatted_datetime();
static void alert_blink_cb(lv_timer_t * timer);
void touchscreen_event_cb(lv_event_t * e);
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data); // <- reemplazada
void lv_create_splash_screen();
void lv_create_config_menu();
void scan_and_show_wifi_list(lv_obj_t * parent);
void show_wifi_keyboard(const char * ssid);
void connect_to_wifi(String ssid, String password);
void lv_create_wifi_menu();
void scan_and_show_wifi_list(lv_obj_t * parent, int max_items);
static void on_espnow_recv(const uint8_t *mac, const uint8_t *incomingData, int len);
static int wifi_channel();
static String mac_to_string(const uint8_t mac[6]);
static int find_sensor_index_by_mac(const uint8_t mac[6]);
static int touch_or_add_sensor(const uint8_t mac[6]);
static void open_name_screen(size_t idx);
static void sensor_btn_clicked_cb(lv_event_t * e);
void kb_name_ok_cb(lv_event_t * e);
void kb_name_cancel_cb(lv_event_t * e);
static void open_sensor_list_screen();
static void populate_sensor_list();
static void sensor_scan_timer_cb(lv_timer_t * t);
static lv_obj_t * build_wifi_style_keyboard(lv_obj_t * parent, lv_obj_t * textarea);
static lv_obj_t * create_page(lv_obj_t *parent);
static void build_common_widgets(lv_obj_t *page, PageWidgets &w, const char *title);
static void build_local_page();
static void ensure_remote_page(size_t idx);
static void update_nav_arrows_pages();
static float mq7_adc_to_ppm(int raw);
static void read_local_sensors();
static void set_status_icon(lv_obj_t *icon, float ppm);
static void go_to_page(int idx, bool anim=false);
static lv_obj_t * make_back_button(lv_obj_t *parent, lv_align_t align, lv_coord_t offx, lv_coord_t offy, lv_event_cb_t cb);
static void ui_timer_cb(lv_timer_t * timer);
static void sensor_timer_cb(lv_timer_t * timer);
static bool rtc_has_valid_time();
static bool wait_time_sync(uint32_t timeout_ms);
static bool sync_time_via_wifi(const String& ssid, const String& pass);
static void ensure_sta_for_scan();

static void refresh_ui_now();
static void set_selected_sensor(int idx);
static void select_next_sensor();
static void select_prev_sensor();
static void auto_rotate_cb(lv_timer_t *t);
static void ensure_auto_rotate_timer();
static void create_alert_bars();
static inline void hide_alert_bars();
static inline void show_alert_bars();
static void cb_back_to_main(lv_event_t * e);
static void cb_back_to_settings(lv_event_t * e);
static void cb_sensor_back(lv_event_t * e);
static void espnow_start();
static void espnow_stop();

static lv_obj_t * text_label_temperature;
static lv_obj_t * text_label_humidity;
static lv_obj_t * text_label_time_location;
static lv_obj_t * text_label_ppm;
static lv_obj_t * image_status_icon;
static lv_obj_t * splash_screen;
static lv_obj_t * main_screen;
static lv_obj_t * config_screen;
static lv_timer_t * splash_timer;
static lv_obj_t * kb;
static lv_obj_t * ta;
static String selected_ssid = "";
static lv_style_t style_btn_close;
static lv_style_t style_btn_ok;
static bool alert_blink_state = false;
static bool alert_active = false;
static lv_obj_t * local_page_root = nullptr;
static volatile bool g_need_page_sync = false;
static lv_obj_t *alert_overlay = nullptr;
static float last_local_t = NAN, last_local_h = NAN, last_local_co = NAN;
static lv_obj_t *alert_top = nullptr, *alert_bottom = nullptr, *alert_left = nullptr, *alert_right = nullptr;
#define ALERT_THICKNESS  6   // <- grosor del marco (ajustable)
static bool espnow_running = false;

static uint32_t nav_quiet_until = 0;
static lv_timer_t * ui_timer = nullptr;
static lv_timer_t * blink_timer = nullptr;
static lv_timer_t * sensor_timer = nullptr;
static inline void pause_main_timers(bool pause) {
  if (ui_timer)     (pause ? lv_timer_pause(ui_timer)     : lv_timer_resume(ui_timer));
  if (blink_timer)  (pause ? lv_timer_pause(blink_timer)  : lv_timer_resume(blink_timer));
  if (sensor_timer) (pause ? lv_timer_pause(sensor_timer) : lv_timer_resume(sensor_timer));
}

// === Protecciones de concurrencia y estado ===
static portMUX_TYPE sensors_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool in_settings = false;

// Soltar toque forzado (evita toques “fantasma” al cambiar de pantalla)
static inline void force_release_touch();

// Navegación
static lv_obj_t *btn_prev = nullptr;
static lv_obj_t *btn_next = nullptr;
static uint32_t last_user_nav_ms = 0;
#define AUTO_ROTATE_MS 0
static lv_timer_t *auto_rotate_timer = nullptr;

// Teclado Wi‑Fi
static bool kb_caps = true;
static const char * KB_MAP_UPPER[] = {
  "1","2","3","4","5","6","7","8","9","0","\n",
  "Q","W","E","R","T","Y","U","I","O","P", LV_SYMBOL_BACKSPACE, "\n",
  "A","S","D","F","G","H","J","K","L",     LV_SYMBOL_UP,        "\n",
  "Z","X","C","V","B","N","M",",",".","!","?","\n",
  LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, NULL
};
static const char * KB_MAP_LOWER[] = {
  "1","2","3","4","5","6","7","8","9","0","\n",
  "q","w","e","r","t","y","u","i","o","p", LV_SYMBOL_BACKSPACE, "\n",
  "a","s","d","f","g","h","j","k","l",     LV_SYMBOL_UP,        "\n",
  "z","x","c","v","b","n","m",",",".","!","?","\n",
  LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, NULL
};
static const lv_buttonmatrix_ctrl_t KB_CTRL_MAP[] = {
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_3,
  LV_BUTTONMATRIX_CTRL_WIDTH_6,
  LV_BUTTONMATRIX_CTRL_WIDTH_3,
};

// =====================================================
// ========= NUEVO: Infraestructura táctil RTOS ========
// =====================================================
struct TouchSample { int16_t x, y; bool pressed; uint32_t ts; };
//static volatile TouchSample g_touch_last = {0,0,false,0};
static TouchSample g_touch_last = {0,0,false,0};
static portMUX_TYPE g_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t g_touch_task = nullptr;

static void touch_task(void *);

static const int   TS_SAMPLES      = 4;    // 3–5
static const int   TS_MAX_JITTER   = 15;   // px
static const int   TS_RELEASE_HOLD = 30;   // ms

static inline int16_t clamp16(int v, int lo, int hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
static int median_small(int *v, int n){
  for(int i=1;i<n;i++){ int k=v[i], j=i-1; while(j>=0 && v[j]>k){ v[j+1]=v[j]; j--; } v[j+1]=k; }
  return v[n/2];
}

// =================== SETUP / LOOP ===================
void setup() {
  String LVGL_Arduino = String("LVGL Library Version: ") + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
  Serial.begin(115200);
  Serial.println(LVGL_Arduino);

  dht.begin();
  analogReadResolution(12);

  // --- RTC: si querés, podés mostrar si ya hay hora válida antes de NTP
  setenv("TZ", "America/Argentina/Buenos_Aires", 1);
  tzset();
  if (rtc_has_valid_time()) {
    Serial.println("[TIME] RTC ya tiene hora válida al arrancar.");
  }

  espnow_start();
  Serial.printf("ESP-NOW listo. Canal actual: %d\n", wifi_channel());

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // LVGL + TFT
  lv_init();
  lv_log_register_print_cb(log_print);

  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
  pinMode(XPT2046_IRQ, INPUT_PULLUP);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  lv_display_t * disp;
  disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

  g_disp = lv_display_get_default();
  SW = lv_display_get_horizontal_resolution(g_disp);
  SH = lv_display_get_vertical_resolution(g_disp);
  Serial.printf("RES after rotation: %d x %d\n", (int)SW, (int)SH);

  // InDev táctil (usa read_cb ligero)
  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);
  lv_indev_set_long_press_repeat_time(indev, 0);
  lv_indev_set_long_press_time(indev, 450);
  lv_indev_set_scroll_limit(indev, 16);

  // ===== NEW: lanzar task táctil en CORE 0 =====
  xTaskCreatePinnedToCore(
    touch_task, "touch_task",
    4096, nullptr,
    12,               // prioridad alta (no máxima)
    &g_touch_task,
    0                 // 0 -> core 0 (GUI/loop suele ir en core 1)
  );

  // UI
  lv_create_splash_screen();
}

void loop() {
  lv_timer_handler();  // procesa LVGL
  delay(5);            // duerme ~5 ms reales
  lv_tick_inc(5);      // informa 5 ms a LVGL
}

void lv_create_main_gui(void) {
  LV_IMAGE_DECLARE(image_settings);

  if (pages) {                // <-- evita duplicados
    lv_scr_load(lv_screen_active());
    return;
  }

  // ROOT horizontal de páginas
  pages = lv_obj_create(lv_screen_active());
  lv_obj_set_style_anim_time(pages, 0, 0);
  lv_obj_set_size(pages, SW, SH);
  lv_obj_align(pages, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_scroll_dir(pages, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pages, LV_SCROLL_SNAP_START);
  lv_obj_set_style_pad_all(pages, 0, 0);
  lv_obj_set_style_pad_row(pages, 0, 0);
  lv_obj_set_style_pad_column(pages, 0, 0);
  lv_obj_set_flex_flow(pages, LV_FLEX_FLOW_ROW);
/*
  if (!alert_overlay) {
  alert_overlay = lv_obj_create(lv_screen_active());
  lv_obj_set_size(alert_overlay, SW, SH);
  lv_obj_align(alert_overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(alert_overlay, LV_OPA_TRANSP, 0);   // sin fondo
  lv_obj_set_style_border_width(alert_overlay, 4, 0);
  lv_obj_set_style_border_color(alert_overlay, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_set_style_shadow_width(alert_overlay, 0, 0);
  //lv_obj_add_flag(alert_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(alert_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(alert_overlay, LV_OBJ_FLAG_HIDDEN);          // empieza oculto
  lv_obj_move_foreground(alert_overlay);
}
*/
  if (!alert_top) create_alert_bars();

  lv_obj_set_scrollbar_mode(pages, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(pages, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_clear_flag(pages, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  // Página 0: LOCAL
  build_local_page();
  // Asegurar que la primera página quede centrada/visible
  lv_obj_update_layout(pages);
  lv_obj_scroll_to_x(pages, 0, LV_ANIM_OFF);
  if (local_page_root) {
    lv_obj_scroll_to_view(local_page_root, LV_ANIM_OFF);
  }

  lv_obj_update_layout(pages);
  g_curr_page = 0;
  go_to_page(0, /*anim=*/false);

  // Botón de configuración (queda sobre la pantalla activa)
  lv_obj_t * btn_settings = lv_image_create(lv_screen_active());
  lv_image_set_src(btn_settings, &image_settings);
  lv_obj_align(btn_settings, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_move_foreground(btn_settings);
  lv_obj_add_event_cb(btn_settings, [](lv_event_t * e) {
    lv_create_config_menu();
  }, LV_EVENT_PRESSED, NULL);

  // Fecha/hora (tu footer actual)
  text_label_time_location = lv_label_create(lv_screen_active());
  String datetime_str = get_formatted_datetime() + " | " + location;
  lv_label_set_text(text_label_time_location, datetime_str.c_str());
  lv_obj_align(text_label_time_location, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_text_font(text_label_time_location, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(text_label_time_location, lv_palette_main(LV_PALETTE_GREY), 0);

  // Timers
  ui_timer = lv_timer_create(ui_timer_cb, UI_REFRESH_MS, NULL);
  lv_timer_ready(ui_timer);

  blink_timer = lv_timer_create(alert_blink_cb, 500, NULL);

  sensor_timer = lv_timer_create(sensor_timer_cb, LOCAL_READ_MS, NULL);
  lv_timer_ready(sensor_timer);

  update_nav_arrows_pages();
  ensure_auto_rotate_timer();
}

// Function to get weather data from the DHT sensor
void get_weather_data() {
  // Si hay un sensor seleccionado, mostramos su info
  if (selectedSensorIndex >= 0 && selectedSensorIndex < (int)sensors.size()) {
    const DiscoveredSensor &s = sensors[selectedSensorIndex];
    bool fresh = (millis() - s.lastSeen) <= SENSOR_STALE_MS;

    // Actualizar SIEMPRE el nombre (no lo borres mientras haya sensor seleccionado)
    if (text_label_sensor_name) {
      lv_label_set_text(text_label_sensor_name, s.name.length() ? s.name.c_str() : s.macStr.c_str());
    }

    // Si los datos son frescos, refrescamos; si no, dejamos lo último que se venía mostrando
    if (fresh && !isnan(s.last.temp) && !isnan(s.last.float_hum)) {
      temperature = String(s.last.temp, 1);
      humidity    = String(s.last.float_hum, 1);
      monoxide    = (!isnan(s.last.mono) && s.last.mono >= 0) ? String(s.last.mono, 1) : "0";
    }
    return; // importante: no caigas al “Sin sensor”
  }

  // Sin sensor seleccionado: estado neutro
  temperature = "--";
  humidity    = "--";
  monoxide    = "--";
  if (text_label_sensor_name) lv_label_set_text(text_label_sensor_name, "Sin sensor");
}

// If logging is enabled, it will inform the user about what is happening in the library
void log_print(lv_log_level_t level, const char * buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}

static void ui_timer_cb(lv_timer_t * timer){
  LV_UNUSED(timer);

  if (g_need_page_sync) {
    // asegurar páginas remotas existentes
    size_t n;
    portENTER_CRITICAL(&sensors_mux);
    n = sensors.size();
    portEXIT_CRITICAL(&sensors_mux);

    for (size_t i = 0; i < n; ++i) ensure_remote_page(i);
    update_nav_arrows_pages();
    go_to_page(g_curr_page, false);
    g_need_page_sync = false;
  }

  // Local (cache de cambios)
  if (local_t != last_local_t && local_page.t) {
    if (isnan(local_t)) lv_label_set_text(local_page.t, "--");
    else                lv_label_set_text_fmt(local_page.t, "%.1f%s", local_t, degree_symbol);
    last_local_t = local_t;
  }
  if (local_h != last_local_h && local_page.h) {
    if (isnan(local_h)) lv_label_set_text(local_page.h, "--");
    else                lv_label_set_text_fmt(local_page.h, "%.1f%%", local_h);
    last_local_h = local_h;
  }
  if (local_co != last_local_co && local_page.co) {
    if (isnan(local_co)) lv_label_set_text(local_page.co, "--");
    else                 lv_label_set_text_fmt(local_page.co, "%.0f ppm", local_co);
    last_local_co = local_co;
  }
  if (local_page.icon) set_status_icon(local_page.icon, isnan(local_co) ? 0.0f : local_co);

  // Remotos (copiar snapshot para iterar fuera del lock largo)
  std::vector<DiscoveredSensor> snap;
  portENTER_CRITICAL(&sensors_mux);
  snap = sensors; // copia liviana (String hace copy-on-write en Arduino)
  portEXIT_CRITICAL(&sensors_mux);

  for (size_t i = 0; i < snap.size(); ++i) {
    const auto &s = snap[i];
    PageWidgets &w = remote_pages[i];
    bool fresh = (millis() - s.lastSeen) <= SENSOR_STALE_MS;

    if (w.name) {
      const String title = s.name.length()? s.name : s.macStr;
      lv_label_set_text(w.name, title.c_str());
    }

    float t  = (fresh && !isnan(s.last.temp))       ? s.last.temp      : NAN;
    float h  = (fresh && !isnan(s.last.float_hum))  ? s.last.float_hum : NAN;
    float co = (fresh && !isnan(s.last.mono))       ? s.last.mono      : NAN;

    if (w.t)  { if (isnan(t))  lv_label_set_text(w.t,  "--"); else lv_label_set_text_fmt(w.t,  "%.1f%s", t,  degree_symbol); }
    if (w.h)  { if (isnan(h))  lv_label_set_text(w.h,  "--"); else lv_label_set_text_fmt(w.h,  "%.1f%%",  h); }
    if (w.co) { if (isnan(co)) lv_label_set_text(w.co, "--"); else lv_label_set_text_fmt(w.co, "%.0f ppm", co); }
    if (w.icon) set_status_icon(w.icon, isnan(co)? 0.0f : co);
  }

  // Footer cada ~1s
  static uint32_t last_time_ms = 0;
  if (text_label_time_location && (lv_tick_get() - last_time_ms) > 1000) {
    lv_label_set_text(text_label_time_location, (get_formatted_datetime() + " | " + location).c_str());
    last_time_ms = lv_tick_get();
  }
}

String get_formatted_datetime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "00/00/0000 00:00";
  }

  char buffer[30];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M", &timeinfo);
  return String(buffer);
}

static void alert_blink_cb(lv_timer_t * timer) {
  LV_UNUSED(timer);
  if (!alert_top) return;

  if (!alert_active) {
    if (!buzzer_muted) digitalWrite(BUZZER_PIN, LOW);
    hide_alert_bars();
    return;
  }

  alert_blink_state = !alert_blink_state;
  if (alert_blink_state) show_alert_bars(); else hide_alert_bars();

  if (!buzzer_muted) digitalWrite(BUZZER_PIN, alert_blink_state ? HIGH : LOW);
  else               digitalWrite(BUZZER_PIN, LOW);
}

void lv_create_splash_screen() {
  LV_IMAGE_DECLARE(image_cleanair);

  splash_screen = lv_screen_active();
  lv_obj_t * img = lv_image_create(splash_screen);
  lv_image_set_src(img, &image_cleanair);
  lv_obj_align(img, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t * label = lv_label_create(splash_screen);
  lv_label_set_text(label, "Air Quality System");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 80);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  splash_timer = lv_timer_create([](lv_timer_t * timer) {
    lv_obj_clean(lv_screen_active());
    lv_create_main_gui();
    main_screen = lv_screen_active();    // <-- AHORA sí guardamos la pantalla principal
    lv_timer_del(timer);
  }, 1000, NULL);
}

void scan_and_show_wifi_list(lv_obj_t * parent, int max_items) {
  availableSSIDs.clear();

  int n = WiFi.scanComplete(); // resultados cacheados del último scan async
  if (n <= 0) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "No se encontraron redes WiFi.");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    return;
  }

  if (n == 0) {
  lv_obj_t * label = lv_label_create(parent);
  lv_label_set_text(label, "No se encontraron redes WiFi.");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
  return;
  }

  static lv_style_t style_wifi_text;
  static bool style_wifi_text_inited = false;
  if (!style_wifi_text_inited) {
    style_wifi_text_inited = true;
    lv_style_init(&style_wifi_text);
    lv_style_set_text_font(&style_wifi_text, &lv_font_montserrat_18);
  }

  int shown = 0;
  for (int i = 0; i < n && shown < max_items; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;     // ignora ocultas/vacías si no querés listarlas

    availableSSIDs.push_back(ssid);

    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_width(btn, lv_pct(70));
    lv_obj_set_height(btn, 32);

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, ssid.c_str());
    lv_obj_center(label);
    lv_obj_add_style(label, &style_wifi_text, 0);

    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
      lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
      lv_obj_t * label = lv_obj_get_child(btn, 0);
      const char * ssid_selected = lv_label_get_text(label);
      show_wifi_keyboard(ssid_selected);
    }, LV_EVENT_CLICKED, NULL);

    shown++;
  }

  // deja los resultados cacheados por si volvés a esta pantalla sin re-escanear
  lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);
}

void show_wifi_keyboard(const char * ssid) {
  pause_main_timers(true);

  lv_indev_reset(NULL, NULL);

  selected_ssid = String(ssid);
  lv_obj_clean(lv_screen_active());

  // Título
  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text_fmt(label, "%s", ssid);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

  // Text Area (password)
  ta = lv_textarea_create(lv_screen_active());
  lv_obj_set_width(ta, lv_pct(90));
  lv_obj_set_height(ta, 50);
  lv_textarea_set_password_mode(ta, true);
  lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_add_state(ta, LV_STATE_FOCUSED);

  // Teclado reutilizable con Shift
  lv_obj_t * kb_local = build_wifi_style_keyboard(lv_screen_active(), ta);

  // OK → conectar
  lv_obj_add_event_cb(kb_local, [](lv_event_t * e) {
    String password = lv_textarea_get_text(ta);
    connect_to_wifi(selected_ssid, password);
  }, LV_EVENT_READY, NULL);

  // Back → volver al menú Wi‑Fi
  lv_obj_add_event_cb(kb_local, [](lv_event_t * e) {
    lv_obj_clean(lv_screen_active());
    lv_create_wifi_menu();
  }, LV_EVENT_CANCEL, NULL);
}

void connect_to_wifi(String ssid, String password) {
  lv_obj_clean(lv_screen_active());

  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text(label, "Sincronizando hora...");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  // Hacemos la sync (bloqueante pero corta)
  bool ok = sync_time_via_wifi(ssid, password);

  if (ok) {
    lv_label_set_text(label, "Hora actualizada ✓");
  } else {
    lv_label_set_text(label, "No se pudo actualizar la hora");
  }

  // Pequeña pausa visual
  delay(1200);

  // Volver a la pantalla principal y reanudar timers/alertas
  lv_scr_load(main_screen);
  lv_indev_reset(NULL, NULL);
  pause_main_timers(false);
  if (alert_active) show_alert_bars(); else hide_alert_bars();
}

void lv_create_config_menu() {
  pause_main_timers(true);
  hide_alert_bars();

  in_settings = true;
  force_release_touch();

  static lv_style_t style_btn_text;
  static bool style_btn_text_inited = false;
  if(!style_btn_text_inited) {
    style_btn_text_inited = true;
    lv_style_init(&style_btn_text);
    lv_style_set_text_font(&style_btn_text, &lv_font_montserrat_28);
  }

  // Crear pantalla de configuración
  config_screen = lv_obj_create(NULL);
  lv_obj_set_size(config_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(config_screen);

  // Título
  lv_obj_t * title = lv_label_create(config_screen);
  lv_label_set_text(title, "Menu de configuracion");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver (sale a principal)
  make_back_button(config_screen, LV_ALIGN_BOTTOM_LEFT, 10, -10, cb_back_to_main);

  // Contenedor para opciones
  lv_obj_t * list = lv_obj_create(config_screen);
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

  // Botón Wi‑Fi
  lv_obj_t * btn_wifi = lv_btn_create(list);
  lv_obj_set_width(btn_wifi, lv_pct(100));
  lv_obj_set_height(btn_wifi, 50);
  lv_obj_t * lbl_wifi = lv_label_create(btn_wifi);
  lv_label_set_text(lbl_wifi, "WiFi");
  lv_obj_center(lbl_wifi);
  lv_obj_add_style(lbl_wifi, &style_btn_text, 0);
  lv_obj_add_event_cb(btn_wifi, [](lv_event_t * e) {
    LV_UNUSED(e);
    lv_create_wifi_menu(); // seguimos en settings
  }, LV_EVENT_CLICKED, NULL);

  // Botón Agregar Sensor
  lv_obj_t * btn_sensor = lv_btn_create(list);
  lv_obj_set_width(btn_sensor, lv_pct(100));
  lv_obj_set_height(btn_sensor, 50);
  lv_obj_t * lbl_sensor = lv_label_create(btn_sensor);
  lv_label_set_text(lbl_sensor, "Agregar sensor");
  lv_obj_center(lbl_sensor);
  lv_obj_add_style(lbl_sensor, &style_btn_text, 0);
  lv_obj_add_event_cb(btn_sensor, [](lv_event_t * e) {
    LV_UNUSED(e);
    open_sensor_list_screen(); // seguimos en settings
  }, LV_EVENT_CLICKED, NULL);
}

void lv_create_wifi_menu() {
  pause_main_timers(true);
  hide_alert_bars();

  in_settings = true;
  force_release_touch();

  // ► Pausar ESP-NOW para que el escaneo sea fiable
  espnow_stop();

  // ► Preparar STA para escanear (NO apagarla)
  ensure_sta_for_scan();

  lv_obj_t * wifi_screen = lv_obj_create(NULL);
  lv_obj_set_size(wifi_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(wifi_screen);

  lv_obj_t * list = lv_obj_create(wifi_screen);
  lv_obj_set_size(list, lv_pct(90), lv_pct(70));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

  // Botón Volver → vuelve a Settings y REANUDA ESP-NOW allí
  make_back_button(wifi_screen, LV_ALIGN_BOTTOM_LEFT, 10, -10, cb_back_to_settings);

  // Contenedor scrollable para la lista
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_style_pad_all(list, 8, 0);
  // Scroll solo vertical y sin “snap”
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);
  lv_obj_set_style_anim_time(list, 0, 0);
  // Quitar borde, sombra y contorno
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_shadow_width(list, 0, 0);
  lv_obj_set_style_outline_width(list, 0, 0);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);

  lv_obj_set_flex_align(list,
    LV_FLEX_ALIGN_START,  // alineación horizontal (main axis para row, cross axis para column)
    LV_FLEX_ALIGN_CENTER,  // alineación vertical (en tu caso no afecta mucho)
    LV_FLEX_ALIGN_CENTER   // alineación de contenido (última línea)
  );

  lv_obj_t * scanning = lv_label_create(list);
  lv_label_set_text(scanning, "Escaneando...");
  lv_obj_center(scanning);

  // --- Escaneo asíncrono real (con parámetros explícitos) ---
  WiFi.scanDelete();
  // async=true, show_hidden=false, passive=true, max_ms_per_chan=180
  WiFi.scanNetworks(true, /*show_hidden*/false, /*passive*/true, /*max_ms_per_chan*/180);

  // Timer que espera el fin del scan sin bloquear (con 1 reintento si falla)
  lv_timer_t * t = lv_timer_create_basic();
  lv_timer_set_period(t, 180);
  lv_timer_set_repeat_count(t, -1);
  lv_timer_set_user_data(t, list);
  lv_timer_set_cb(t, [](lv_timer_t * t) {
    lv_obj_t * parent_list = (lv_obj_t *)lv_timer_get_user_data(t);
    static bool retried = false;

    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    lv_obj_clean(parent_list);

    if (n == WIFI_SCAN_FAILED) {
      // Primer intento falló → hacemos un "soft restart" y reintentamos una vez
      if (!retried) {
        retried = true;
        WiFi.scanDelete();
        // “Soft restart” del driver
        esp_wifi_stop(); esp_wifi_start(); delay(50);
        // Reintento con mismos parámetros
        WiFi.scanNetworks(true, false, true, 200);
        // Mostramos “reintentando...”
        lv_obj_t * lbl = lv_label_create(parent_list);
        lv_label_set_text(lbl, "Reintentando escaneo...");
        lv_obj_center(lbl);
        return;
      }
      // Falló incluso tras retry
      lv_obj_t * lbl = lv_label_create(parent_list);
      lv_label_set_text(lbl, "Error de escaneo.\nReintentá o acercate al router.");
      lv_obj_center(lbl);
      lv_timer_del(t);
      return;
    }

    // OK o 0 redes: mostramos (0 → “No se encontraron redes”)
    scan_and_show_wifi_list(parent_list, WIFI_LIST_LIMIT);
    lv_timer_del(t);
  });
}

static void on_espnow_recv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!mac || len != sizeof(struct_message)) return;

  // Si estamos en settings: NO crear sensores nuevos (evita realocaciones del vector)
  if (in_settings) {
    int idx = find_sensor_index_by_mac(mac);
    if (idx < 0) return; // ignorá altas nuevas
    portENTER_CRITICAL(&sensors_mux);
    memcpy(&sensors[idx].last, incomingData, sizeof(struct_message));
    sensors[idx].lastSeen = millis();
    portEXIT_CRITICAL(&sensors_mux);
  } else {
    portENTER_CRITICAL(&sensors_mux);
    int idx = touch_or_add_sensor(mac);  // puede push_back
    memcpy(&sensors[idx].last, incomingData, sizeof(struct_message));
    sensors[idx].lastSeen = millis();
    portEXIT_CRITICAL(&sensors_mux);
  }

  // Log opcional, ACK, etc. (sin tocar el vector)
  const struct_message &in = *(const struct_message*)incomingData;
  Serial.printf("[ESP-NOW] RX %02X:%02X:%02X:%02X:%02X:%02X | S:%c T:%.1f H:%.1f CO:%.1f\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], in.sensor, in.temp, in.float_hum, in.mono);

  if (in.ackRequired) {
    if (!esp_now_is_peer_exist(mac)) {
      esp_now_peer_info_t peer{};
      memcpy(peer.peer_addr, mac, 6);
      peer.channel = 0;
      peer.encrypt = false;
      esp_now_add_peer(&peer);
    }
    struct_message ack{};
    ack.sensor = 'R';
    ack.temp = in.temp;
    ack.float_hum = in.float_hum;
    ack.mono = in.mono;
    ack.ackRequired = false;
    ack.isAck = true;
    esp_now_send(mac, (uint8_t*)&ack, sizeof(ack));
  }

  if (selectedSensorIndex < 0) {
    // elegir alguno existente, sin leer el vector dentro del lock
    portENTER_CRITICAL(&sensors_mux);
    if (!sensors.empty()) selectedSensorIndex = 0;
    portEXIT_CRITICAL(&sensors_mux);
  }

  g_need_page_sync = true;   // pedir a la UI que sincronice páginas
}

// (opcional) para debugging canal WiFi
static int wifi_channel() {
  wifi_second_chan_t sc;
  uint8_t ch = 0;
  esp_wifi_get_channel(&ch, &sc);
  return (int)ch;
}

// Helpers
static String mac_to_string(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static int find_sensor_index_by_mac(const uint8_t mac[6]) {
  for (size_t i = 0; i < sensors.size(); ++i) {
    if (memcmp(sensors[i].mac, mac, 6) == 0) return (int)i;
  }
  return -1;
}

static int touch_or_add_sensor(const uint8_t mac[6]) {
  int idx = find_sensor_index_by_mac(mac);
  if (idx < 0) {
    DiscoveredSensor s{};
    memcpy(s.mac, mac, 6);
    s.macStr  = mac_to_string(mac);
    s.name    = "";          // sin nombre por defecto
    s.lastSeen = millis();
    sensors.push_back(s);
    return (int)sensors.size() - 1;
  } else {
    sensors[idx].lastSeen = millis();
    return idx;
  }
}

// Abrir pantalla de lista de sensores (y empezar escaneo)
static void open_name_screen(size_t idx) {
  lv_obj_t * scr2 = lv_obj_create(NULL);
  lv_obj_set_size(scr2, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(scr2);

  lv_obj_t * t2 = lv_label_create(scr2);
  lv_label_set_text_fmt(t2, "Nombrar sensor\n%s", sensors[idx].macStr.c_str());
  lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t * ta_local = lv_textarea_create(scr2);
  lv_obj_set_width(ta_local, lv_pct(90));
  lv_obj_set_height(ta_local, 50);
  lv_obj_align(ta_local, LV_ALIGN_TOP_MID, 0, 60);
  lv_textarea_set_placeholder_text(ta_local, "Ej: Cocina");
  if (sensors[idx].name.length()) lv_textarea_set_text(ta_local, sensors[idx].name.c_str());
  lv_obj_add_state(ta_local, LV_STATE_FOCUSED);

  // Teclado estilo Wi‑Fi (mismo mapa y botones Back/OK)
  lv_obj_t * kb_local = build_wifi_style_keyboard(scr2, ta_local);

  // Guardamos el índice en user_data del teclado
  lv_obj_set_user_data(kb_local, (void*)idx);

  // OK = LV_EVENT_READY
  lv_obj_add_event_cb(kb_local, [](lv_event_t * e3) {
    lv_obj_t * kb_ = (lv_obj_t *)lv_event_get_target(e3);
    size_t idx_ = (size_t)lv_obj_get_user_data(kb_);
    lv_obj_t * ta_ = (lv_obj_t *)lv_keyboard_get_textarea(kb_);

    sensors[idx_].name = String(lv_textarea_get_text(ta_));
    selectedSensorIndex = (int)idx_;

    lv_scr_load(main_screen);
    pause_main_timers(false);
    if (text_label_sensor_name) {
      const String &n = sensors[idx_].name;
      lv_label_set_text(text_label_sensor_name, n.length() ? n.c_str() : sensors[idx_].macStr.c_str());
    }
  }, LV_EVENT_READY, NULL);

  // Back = LV_EVENT_CANCEL
  lv_obj_add_event_cb(kb_local, [](lv_event_t * e3) {
    open_sensor_list_screen(); // vuelve a la lista y sigue escaneando
  }, LV_EVENT_CANCEL, NULL);
}

// Al tocar un sensor de la lista: pantalla para nombrar y seleccionar
static void sensor_btn_clicked_cb(lv_event_t * e) {
  size_t idx = (size_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
  open_name_screen(idx);
}

// Callback del botón OK del teclado de “nombrar”
void kb_name_ok_cb(lv_event_t * e) {
  NameCtx * ctx = (NameCtx *)lv_event_get_user_data(e);
  if (!ctx) return;

  // Guardar nombre y seleccionar sensor activo
  sensors[ctx->idx].name = String(lv_textarea_get_text(ctx->ta));
  selectedSensorIndex = (int)ctx->idx;

  // Volver a principal y actualizar etiqueta
  lv_scr_load(main_screen);
  pause_main_timers(false);
  if (text_label_sensor_name) {
    const String &n = sensors[ctx->idx].name;
    lv_label_set_text(text_label_sensor_name, n.length() ? n.c_str() : sensors[ctx->idx].macStr.c_str());
  }
}

// Callback del botón CANCEL del teclado
void kb_name_cancel_cb(lv_event_t * e) {
  // Volver al menú de configuración (simple)
  lv_create_config_menu();
}

// Crea/abre la pantalla con la lista de sensores detectados
static void open_sensor_list_screen() {
  hide_alert_bars();

  if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
  if (sensor_list_screen) { lv_obj_del(sensor_list_screen); sensor_list_screen = nullptr; }

  sensor_list_screen = lv_obj_create(NULL);
  lv_obj_set_size(sensor_list_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(sensor_list_screen);

  lv_indev_reset(NULL, NULL);
  force_release_touch();

  // Título
  lv_obj_t * title = lv_label_create(sensor_list_screen);
  lv_label_set_text(title, "Sensores disponibles");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver
  make_back_button(sensor_list_screen, LV_ALIGN_BOTTOM_LEFT, 10, -10, cb_sensor_back);
 

  // Contenedor scrollable (lista)
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

  // Poblado inicial
  populate_sensor_list();

  // Timer de refresco: si no hay sensores (o aunque haya), seguí actualizando cada 2s
  sensor_scan_timer = lv_timer_create(sensor_scan_timer_cb, 2000, NULL);
}

// Llena (o recarga) la lista según lo visto recientemente
static void populate_sensor_list() {
  if (!sensor_list_container) return;

  lv_obj_clean(sensor_list_container);

  // Snapshot para iterar sin lock largo
  std::vector<DiscoveredSensor> snap;
  portENTER_CRITICAL(&sensors_mux);
  snap = sensors;
  portEXIT_CRITICAL(&sensors_mux);

  int shown = 0;
  uint32_t now = millis();

  for (size_t i = 0; i < snap.size(); ++i) {
    if (now - snap[i].lastSeen > SENSOR_STALE_MS) continue; // muy viejo

    lv_obj_t * btn = lv_btn_create(sensor_list_container);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 40);

    String line = (snap[i].name.length() ? snap[i].name : snap[i].macStr);
    line += "   ";
    line += String(snap[i].last.temp,1) + "°C  ";
    line += String(snap[i].last.float_hum,1) + "%  ";
    line += String(snap[i].last.mono,0) + "ppm";

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, line.c_str());
    lv_obj_center(lbl);

    // Guardar índice real del vector principal (buscamos por MAC al click)
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
      // leemos el texto (o podrías guardar el MAC en user_data)
      lv_obj_t *btn = (lv_obj_t*)lv_event_get_target(e);
      lv_obj_t *lbl = lv_obj_get_child(btn, 0);
      String line = lv_label_get_text(lbl);

      // extraer algo único; acá buscamos por MAC/nombre exacto:
      // para simplificar, volvemos a abrir la lista y usamos el índice directo por posición
      // (si querés exactitud 100%, guarda el MAC en user_data)
      size_t idx_click = lv_obj_get_index(btn); // posición visual
      // mapear posición visual a índice real actual:
      int real_idx = -1;

      // snapshot actual
      std::vector<DiscoveredSensor> snap2;
      portENTER_CRITICAL(&sensors_mux);
      snap2 = sensors;
      portEXIT_CRITICAL(&sensors_mux);

      // contar visibles de nuevo y elegir el idx_click‑ésimo
      int count_vis = 0;
      for (size_t k=0;k<snap2.size();++k){
        if (millis() - snap2[k].lastSeen > SENSOR_STALE_MS) continue;
        if (count_vis == (int)idx_click) { real_idx = (int)k; break; }
        count_vis++;
      }
      if (real_idx >= 0) open_name_screen((size_t)real_idx);
    }, LV_EVENT_CLICKED, NULL);

    shown++;
  }

  if (shown == 0) {
    lv_obj_t * lbl = lv_label_create(sensor_list_container);
    lv_label_set_text(lbl, "Buscando sensores...\nAsegurate de que esten transmitiendo.");
    lv_obj_center(lbl);
  }
}

// Timer que repuebla la lista (para “seguir buscando”)
static void sensor_scan_timer_cb(lv_timer_t * t) {
  LV_UNUSED(t);
  // Si el usuario salió de la pantalla, frenamos
  if (!sensor_list_screen || !sensor_list_container) {
    if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
    return;
  }
  populate_sensor_list();
}

// ---- Teclado estilo Wi‑Fi (Shift ↑ NO escribe en el textarea) ----
// ---- Teclado estilo Wi‑Fi (Shift ↑ NO se escribe porque el textarea lo rechaza) ----
static lv_obj_t * build_wifi_style_keyboard(lv_obj_t * parent, lv_obj_t * textarea) {
    // Crear teclado
    lv_obj_t * kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, SCREEN_HEIGHT, SCREEN_WIDTH / 2);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Estado inicial: MAYÚSCULAS en mapa USER_1
    kb_caps = true;
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, KB_MAP_UPPER, KB_CTRL_MAP);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);

    // Vincular textarea
    lv_keyboard_set_textarea(kb, textarea);

    // Aceptar solo estos caracteres en el textarea (ajustá a gusto)
    static const char * ALLOWED =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        " .,;-_!?#@$/\\\"'()[]{}=+*<>|%&:^~`";
    lv_textarea_set_accepted_chars(textarea, ALLOWED);

    // Estilo del teclado (teclas más compactas)
    static lv_style_t style_kb;
    static bool style_inited = false;
    if (!style_inited) {
        style_inited = true;
        lv_style_init(&style_kb);
        lv_style_set_pad_row(&style_kb, 2);
        lv_style_set_pad_column(&style_kb, 2);
        lv_style_set_height(&style_kb, 35);
    }
    lv_obj_add_style(kb, &style_kb, 0);

    // Sin animación para sensación más “snappy” al escribir
    lv_obj_set_style_anim_time(kb, 0, 0);
    lv_obj_set_style_anim_time(textarea, 0, 0);

    // === Anti-duplicados + SHIFT correcto ===
    lv_obj_add_event_cb(kb, [](lv_event_t * e){
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

        lv_obj_t * kb_ = (lv_obj_t *)lv_event_get_target(e);

        // ID del botón actual (API LVGL 9.3)
        uint16_t id = lv_buttonmatrix_get_selected_button(kb_);
        if (id == 0xFFFF) return; // NONE (evita macros faltantes)

        // Anti-duplicado: ignora la MISMA tecla repetida en <120 ms
        static uint16_t last_id = 0xFFFF;
        static uint32_t last_ts = 0;
        uint32_t now = lv_tick_get();
        if (id == last_id && (now - last_ts) < 120) {
            lv_event_stop_bubbling(e);
            lv_event_stop_processing(e);
            return;
        }
        last_id = id;
        last_ts = now;

        // Texto de la tecla (API LVGL 9.3)
        const char * txt = lv_buttonmatrix_get_button_text(kb_, id);
        if (!txt) return;

        // SHIFT: alterna mapa y NO escribe nada en el textarea
        if (strcmp(txt, LV_SYMBOL_UP) == 0 || strcmp(txt, "Shift") == 0) {
            kb_caps = !kb_caps;
            lv_keyboard_set_map(
                kb_, LV_KEYBOARD_MODE_USER_1,
                kb_caps ? KB_MAP_UPPER : KB_MAP_LOWER,
                KB_CTRL_MAP
            );
            lv_event_stop_bubbling(e);
            lv_event_stop_processing(e);
            return;
        }
        // Cualquier otra tecla: dejar fluir hacia el textarea
    }, LV_EVENT_VALUE_CHANGED, NULL);

    return kb;
}

static void refresh_ui_now() {
  get_weather_data();
  if (text_label_temperature)    lv_label_set_text(text_label_temperature, String("      " + temperature + degree_symbol).c_str());
  if (text_label_humidity)       lv_label_set_text(text_label_humidity,    String("   " + humidity + "%").c_str());
  if (text_label_ppm)            lv_label_set_text(text_label_ppm,         String("   " + monoxide + " ppm").c_str());
  if (text_label_time_location)  lv_label_set_text(text_label_time_location, (get_formatted_datetime() + " | " + location).c_str());
}

static void set_selected_sensor(int idx) {
  if (sensors.empty()) {
    selectedSensorIndex = -1;
    refresh_ui_now();
    return;
  }
  // wrap-around
  if (idx < 0) idx = (int)sensors.size() - 1;
  if (idx >= (int)sensors.size()) idx = 0;

  selectedSensorIndex = idx;

  // actualizar nombre arriba
  if (text_label_sensor_name) {
    const auto &s = sensors[selectedSensorIndex];
    const String &n = s.name.length() ? s.name : s.macStr;
    lv_label_set_text(text_label_sensor_name, n.c_str());
  }

  refresh_ui_now();
}

static void select_next_sensor() { set_selected_sensor(selectedSensorIndex + 1); }
static void select_prev_sensor() { set_selected_sensor(selectedSensorIndex - 1); }

static void auto_rotate_cb(lv_timer_t *t) {
  LV_UNUSED(t);
  if (AUTO_ROTATE_MS <= 0) return;
  if (sensors.size() < 2)  return;
  // Pausa 5s tras interacción manual
  if (millis() - last_user_nav_ms < 5000) return;
  select_next_sensor();
}

static void ensure_auto_rotate_timer() {
  if (AUTO_ROTATE_MS > 0 && !auto_rotate_timer) {
    auto_rotate_timer = lv_timer_create(auto_rotate_cb, AUTO_ROTATE_MS, NULL);
  }
}

static lv_obj_t * create_page(lv_obj_t *parent) {
  lv_obj_t * page = lv_obj_create(parent);

  lv_obj_set_size(page, SW, SH);            // <-- 1 pantalla exacta
  lv_obj_set_style_min_width(page, SW, 0);  // <-- por si el flex intenta encoger
  // lv_obj_set_flex_grow(page, 0);         // asegurate de NO usar grow=1

  lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_shadow_width(page, 0, 0);
  return page;
}

static void build_common_widgets(lv_obj_t *page, PageWidgets &w, const char *title) {
  LV_IMAGE_DECLARE(image_weather_temperature);
  LV_IMAGE_DECLARE(image_weather_humidity);
  LV_IMAGE_DECLARE(image_monoxide);
  LV_IMAGE_DECLARE(image_cleanair);

  // Título (nombre)
  w.name = lv_label_create(page);
  lv_label_set_text(w.name, title);
  lv_obj_align(w.name, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_font(w.name, &lv_font_montserrat_22, 0);

  // Icono de estado
  w.icon = lv_image_create(page);
  lv_image_set_src(w.icon, &image_cleanair);
  lv_obj_align(w.icon, LV_ALIGN_CENTER, -100, -10);

  // Temp
  lv_obj_t * i_t = lv_image_create(page);
  lv_image_set_src(i_t, &image_weather_temperature);
  lv_obj_align(i_t, LV_ALIGN_CENTER, 30, -60);

  w.t = lv_label_create(page);
  lv_label_set_text(w.t, "--");
  lv_obj_align(w.t, LV_ALIGN_CENTER, 95, -60);
  lv_obj_set_style_text_font(w.t, &lv_font_montserrat_22, 0);

  // Humedad
  lv_obj_t * i_h = lv_image_create(page);
  lv_image_set_src(i_h, &image_weather_humidity);
  lv_obj_align(i_h, LV_ALIGN_CENTER, 30, 15);

  w.h = lv_label_create(page);
  lv_label_set_text(w.h, "--");
  lv_obj_align(w.h, LV_ALIGN_CENTER, 95, 15);
  lv_obj_set_style_text_font(w.h, &lv_font_montserrat_22, 0);

  // CO
  lv_obj_t * i_co = lv_image_create(page);
  lv_image_set_src(i_co, &image_monoxide);
  lv_obj_align(i_co, LV_ALIGN_CENTER, 30, 80);

  w.co = lv_label_create(page);
  lv_label_set_text(w.co, "--");
  lv_obj_align(w.co, LV_ALIGN_CENTER, 120, 80);
  lv_obj_set_style_text_font(w.co, &lv_font_montserrat_22, 0);
}

static void build_local_page() {
  lv_obj_t * page = create_page(pages);
  local_page_root = page;                 // <-- guardar
  build_common_widgets(page, local_page, "LOCAL");
}

static void ensure_remote_page(size_t idx) {
  while (remote_pages.size() < sensors.size()) remote_pages.push_back(PageWidgets{});
  PageWidgets &w = remote_pages[idx];
  if (w.name) return;  // ya creada

  lv_obj_t * page = create_page(pages);
  String title = sensors[idx].name.length()? sensors[idx].name : sensors[idx].macStr;
  build_common_widgets(page, w, title.c_str());
}

static void update_nav_arrows_pages() {
  // páginas = 1 (local) + sensores “visibles” (podés filtrar por lastSeen si querés)
  size_t pages_count = 1 + sensors.size();
  bool show = pages_count >= 2;

  // Flecha izquierda
  if (!btn_prev) {
    btn_prev = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn_prev, 36, 36);
    lv_obj_align(btn_prev, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_add_event_cb(btn_prev, [](lv_event_t *){
      last_user_nav_ms = millis();
      go_to_page(g_curr_page - 1, false);
    }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_prev);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_center(lbl);
  }
  // Flecha derecha
  if (!btn_next) {
    btn_next = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn_next, 36, 36);
    lv_obj_align(btn_next, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(btn_next, [](lv_event_t *){
      last_user_nav_ms = millis();
      go_to_page(g_curr_page + 1, false);
    }, LV_EVENT_PRESSED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_next);
    lv_label_set_text(lbl, LV_SYMBOL_RIGHT);
    lv_obj_center(lbl);
  }

  if (btn_prev) (show ? lv_obj_clear_flag(btn_prev, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_HIDDEN));
  if (btn_next) (show ? lv_obj_clear_flag(btn_next, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(btn_next, LV_OBJ_FLAG_HIDDEN));
}

static float mq7_adc_to_ppm(int raw) {
  // *** Placeholder simple ***
  // Ajustá con tu calibración: curva o tabla real del MQ7.
  // Por ahora, mapeo lineal aproximado 0..4095 -> 0..500 ppm
  return (float)raw * (500.0f / 4095.0f);
}

static void read_local_sensors() {
  // DHT
  float t = dht.readTemperature();   // °C
  float h = dht.readHumidity();
  if (!isnan(t)) local_t = t;
  if (!isnan(h)) local_h = h;

  // MQ7
  int raw = analogRead(MQ7_PIN);
  local_co = mq7_adc_to_ppm(raw);
}

static void set_status_icon(lv_obj_t *icon, float ppm) {
  LV_IMAGE_DECLARE(image_cleanair);
  LV_IMAGE_DECLARE(image_warning);
  LV_IMAGE_DECLARE(image_alert);

  if (ppm > 100.0f) {
    lv_image_set_src(icon, &image_alert);
  } else if (ppm > 10.0f) {
    lv_image_set_src(icon, &image_warning);
  } else {
    lv_image_set_src(icon, &image_cleanair);
  }
}

static void go_to_page(int idx, bool anim) {
  if (!pages) return;

  // total = 1 (LOCAL) + cantidad de sensores
  int total = 1 + (int)sensors.size();

  if (idx < 0) idx = 0;
  if (idx >= total) idx = total - 1;

  g_curr_page = idx;

  // Cada "page" ocupa 100% del ancho del contenedor
  lv_coord_t w = lv_obj_get_width(pages);
  lv_obj_scroll_to_x(pages, (lv_coord_t)(idx * w), anim ? LV_ANIM_ON : LV_ANIM_OFF);

  // Habilitar/deshabilitar flechas en los extremos
  bool at_first = (idx == 0);
  bool at_last  = (idx == total - 1);

  if (btn_prev) {
    if (at_first) lv_obj_add_state(btn_prev, LV_STATE_DISABLED);
    else          lv_obj_clear_state(btn_prev, LV_STATE_DISABLED);
  }
  if (btn_next) {
    if (at_last)  lv_obj_add_state(btn_next, LV_STATE_DISABLED);
    else          lv_obj_clear_state(btn_next, LV_STATE_DISABLED);
  }
}

// Botón “volver” reutilizable con flecha < (LVGL 9.3)
static lv_obj_t * make_back_button(lv_obj_t *parent, lv_align_t align, lv_coord_t offx, lv_coord_t offy, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 36, 36);                  // mismo tamaño que flechas de sensores
  lv_obj_align(btn, align, offx, offy);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  // (opcional) estilo simple
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 4, 0);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, LV_SYMBOL_LEFT);
  lv_obj_center(lbl);
  return btn;
}

static void sensor_timer_cb(lv_timer_t * timer) {
  LV_UNUSED(timer);
  // Solo leer hardware y actualizar variables globales (local_t, local_h, local_co).
  read_local_sensors();

  // Si querés, podés actualizar acá el estado de alerta (depende de local_co).
  float ppm = isnan(local_co) ? 0.0f : local_co;
  if (ppm > 100.0f) {
    if (!alert_active) buzzer_muted = false;
    alert_active = true;
  } else if (ppm > 10.0f) {
    alert_active = false;
    alert_blink_state = false;
    buzzer_muted = false;
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    alert_active = false;
    alert_blink_state = false;
    buzzer_muted = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// === REEMPLAZO: read_cb sin SPI ===
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  LV_UNUSED(indev);

  static bool     pressed_prev   = false;
  static uint32_t pressed_since  = 0;     // <-- sólo marca inicio de una pulsación
  static int16_t  last_x = 0, last_y = 0;

  // Ventanas de tiempo
  const uint32_t HOLD_TO_RELEASE_MS = 30;   // suaviza el “soltar”
  const uint32_t SAFETY_UP_MS       = 350;  // fuerza soltar si se “pega”

  // Copia atómica del último sample del task
  TouchSample s;
  portENTER_CRITICAL(&g_touch_mux);
  s = g_touch_last;
  portEXIT_CRITICAL(&g_touch_mux);

  uint32_t now = millis();
  bool pressed = s.pressed;
  int16_t rx = s.x, ry = s.y;

  // transiciones
  if (pressed && !pressed_prev) {
    // recién apretó: arrancamos cronómetro
    pressed_since = now;
  } else if (!pressed && pressed_prev) {
    // recién soltó: limpiamos cronómetro
    pressed_since = 0;
  }

  // 1) “release hold” cortito para suavizar soltadas reales
  if (!pressed && pressed_prev && (now - pressed_since) <= HOLD_TO_RELEASE_MS && pressed_since != 0) {
    pressed = true;         // mantenemos PRESSED un ratito más
    rx = last_x; ry = last_y;
  }

  // 2) SAFETY RELEASE: si lleva mucho apretado continuo, forzamos un RELEASE
  if (pressed && pressed_since != 0 && (now - pressed_since) > SAFETY_UP_MS) {
    // Emitimos UN ciclo de RELEASE para destrabar el UI
    data->state   = LV_INDEV_STATE_RELEASED;
    data->point.x = last_x;
    data->point.y = last_y;

    // Limpiamos el estado compartido para cortar la “pulsación eterna”
    force_release_touch();

    // Simulamos que quedó suelto tras este frame
    pressed_prev  = false;
    pressed_since = 0;
    return;
  }

  // 3) Flujo normal
  if (pressed) {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = rx;
    data->point.y = ry;
    last_x = rx; last_y = ry;
    if (alert_active) { buzzer_muted = true; digitalWrite(BUZZER_PIN, LOW); }
  } else {
    data->state   = LV_INDEV_STATE_RELEASED;
    data->point.x = last_x;
    data->point.y = last_y;
  }

  pressed_prev = pressed;
}

static void touch_task(void *){
  pinMode(XPT2046_IRQ, INPUT_PULLUP);
  for(;;){
    //bool raw_pressed = touchscreen.tirqTouched() || touchscreen.touched();
    bool raw_pressed = (digitalRead(XPT2046_IRQ) == LOW);  // <<-- más rápido, sin SPI

    if (raw_pressed) {
      int xs[TS_SAMPLES], ys[TS_SAMPLES], n=0;

      for (int i=0;i<TS_SAMPLES;++i){
        TS_Point p = touchscreen.getPoint();
        float ax=0.001f, bx=-0.130f, dx=498.426f;
        float ay=-0.087f, by=0.001f,  dy=339.434f;
        int x = (int)(ay * p.x + by * p.y + dy);
        int y = (int)(ax * p.x + bx * p.y + dx);
        xs[n] = clamp16(x, 0, SCREEN_WIDTH  - 1);
        ys[n] = clamp16(y, 0, SCREEN_HEIGHT - 1);
        n++;
        ets_delay_us(150);
      }

      int xmin=xs[0], xmax=xs[0], ymin=ys[0], ymax=ys[0];
      for(int i=1;i<n;i++){ if(xs[i]<xmin)xmin=xs[i]; if(xs[i]>xmax)xmax=xs[i]; if(ys[i]<ymin)ymin=ys[i]; if(ys[i]>ymax)ymax=ys[i]; }
      bool ok = ((xmax-xmin)<=TS_MAX_JITTER) && ((ymax-ymin)<=TS_MAX_JITTER);

      if (ok){
        int x_med = median_small(xs, n);
        int y_med = median_small(ys, n);
        portENTER_CRITICAL(&g_touch_mux);
        g_touch_last.x = (int16_t)x_med;
        g_touch_last.y = (int16_t)y_med;
        g_touch_last.pressed = true;
        g_touch_last.ts = millis();
        portEXIT_CRITICAL(&g_touch_mux);
      } else {
        portENTER_CRITICAL(&g_touch_mux);
        g_touch_last.pressed = false;     // mantené x/y previos
        g_touch_last.ts = millis();
        portEXIT_CRITICAL(&g_touch_mux);
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      portENTER_CRITICAL(&g_touch_mux);
      g_touch_last.pressed = false;
      g_touch_last.ts = millis();
      portEXIT_CRITICAL(&g_touch_mux);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

static void create_alert_bars() {
  if (alert_top) return;

  lv_obj_t *top_layer = lv_layer_top();

  auto mkbar = [&](lv_obj_t **out) {
    *out = lv_obj_create(top_layer);
    lv_obj_remove_style_all(*out);
    lv_obj_set_style_bg_opa(*out, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(*out, lv_palette_main(LV_PALETTE_RED), 0);

    // No layout, no clickable, arranca oculto
    lv_obj_add_flag(*out, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(*out, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(*out, LV_OBJ_FLAG_HIDDEN);

    // ► Clave: nunca participar del hit-test (LVGL 8/9)
    lv_obj_add_flag(*out, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(*out, [](lv_event_t * e){
      if (lv_event_get_code(e) == LV_EVENT_HIT_TEST) {
        lv_hit_test_info_t *info = (lv_hit_test_info_t *)lv_event_get_param(e);
        info->res = false;  // nunca “tocado”
      }
    }, LV_EVENT_HIT_TEST, NULL);
  };

  mkbar(&alert_top);
  lv_obj_set_size(alert_top, lv_pct(100), ALERT_THICKNESS);
  lv_obj_align(alert_top, LV_ALIGN_TOP_MID, 0, 0);

  mkbar(&alert_bottom);
  lv_obj_set_size(alert_bottom, lv_pct(100), ALERT_THICKNESS);
  lv_obj_align(alert_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);

  mkbar(&alert_left);
  lv_obj_set_size(alert_left, ALERT_THICKNESS, lv_pct(100));
  lv_obj_align(alert_left, LV_ALIGN_LEFT_MID, 0, 0);

  mkbar(&alert_right);
  lv_obj_set_size(alert_right, ALERT_THICKNESS, lv_pct(100));
  lv_obj_align(alert_right, LV_ALIGN_RIGHT_MID, 0, 0);
}

static inline void hide_alert_bars() {
  if (!alert_top) return;
  lv_obj_add_flag(alert_top,    LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(alert_bottom, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(alert_left,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(alert_right,  LV_OBJ_FLAG_HIDDEN);
}

static inline void show_alert_bars() {
  if (!alert_top) return;
  lv_obj_clear_flag(alert_top,    LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(alert_bottom, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(alert_left,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(alert_right,  LV_OBJ_FLAG_HIDDEN);
}

// Volver a la pantalla principal (desde Settings)
static void cb_back_to_main(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_obj_add_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_DISABLED);
  force_release_touch();
  lv_async_call([](void *) {
    in_settings = false;
    lv_scr_load(main_screen);
    pause_main_timers(false);
    if (alert_active) show_alert_bars(); else hide_alert_bars();
    // ► reanudar ESP-NOW
    espnow_start();
  }, NULL);
}

// Volver a Settings (desde submenús como Wi‑Fi)
static void cb_back_to_settings(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_obj_add_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_DISABLED);
  force_release_touch();
  // limpiar resultados del último scan
  WiFi.scanDelete();
  // ► reanudar ESP-NOW al volver al menú
  espnow_start();
  lv_async_call([](void *) {
    lv_create_config_menu();
  }, NULL);
}

// Volver a Settings desde la lista de sensores
static void cb_sensor_back(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_obj_add_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_DISABLED);
  force_release_touch();
  if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
  if (sensor_list_screen) { lv_obj_del(sensor_list_screen); sensor_list_screen = nullptr; }
  sensor_list_container = nullptr;
  // (No hace falta parar ESP-NOW aquí, no lo tocamos en esta pantalla)
  lv_async_call([](void *) {
    lv_create_config_menu();
  }, NULL);
}

static inline void force_release_touch() {
  // Libera al indev de LVGL
  lv_indev_reset(NULL, NULL);
  // Limpia el último sample de nuestra tarea táctil
  portENTER_CRITICAL(&g_touch_mux);
  g_touch_last.pressed = false;
  g_touch_last.ts = millis();
  portEXIT_CRITICAL(&g_touch_mux);
}

static void espnow_start() {
  if (espnow_running) return;
  WiFi.mode(WIFI_STA);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(on_espnow_recv);
    espnow_running = true;
    Serial.println("[ESP-NOW] start");
  } else {
    Serial.println("[ESP-NOW] init FAILED");
  }
}

static void espnow_stop() {
  if (!espnow_running) return;
  esp_now_deinit();
  espnow_running = false;
  Serial.println("[ESP-NOW] stop");
}

// ====== HORA / RTC ======
static bool rtc_has_valid_time() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return false;
  // tm_year es años desde 1900; si es < 120 (~2020) probablemente no hay NTP
  return (t.tm_year >= 120);
}

// Bloquea hasta que SNTP actualice (con tope de tiempo)
static bool wait_time_sync(uint32_t timeout_ms) {
  uint32_t start = millis();
  struct tm t;
  while (millis() - start < timeout_ms) {
    if (getLocalTime(&t, 50) && t.tm_year >= 120) return true;
    delay(100);
  }
  return false;
}

// Sincroniza hora via Wi-Fi y deja la hora en RTC; luego apaga Wi-Fi y vuelve a ESP-NOW
static bool sync_time_via_wifi(const String& ssid, const String& pass) {
  Serial.println("[TIME] Iniciando sync NTP...");

  // Pausar ESP-NOW (libera el radio para el scan/assoc)
  espnow_stop();

  // Modo STA limpio
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  WiFi.begin(ssid.c_str(), pass.c_str());

  // Esperar conexión (máx ~10s)
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TIME] No se pudo conectar al Wi-Fi para NTP");
    // Dejamos Wi-Fi OFF y reanudamos ESP-NOW
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    espnow_start();
    return false;
  }
  
  // Limpia cualquier TZ previa para que no interfiera
  setenv("TZ", "UTC0", 1);
  tzset();

  // Configurar zona horaria + servidores NTP y esperar sync
  // (Ajustá tu TZ si hace falta; estabas usando "GMT+3")
  //configTzTime("America/Argentina/Buenos_Aires", "pool.ntp.org", "time.nist.gov");
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  bool ok = wait_time_sync(8000);  // esperar hasta 8s

  if (ok) {
    struct tm now;
    getLocalTime(&now, 200);
    char buf[40];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &now);
    Serial.printf("[TIME] NTP OK -> %s\n", buf);
  } else {
    Serial.println("[TIME] NTP timeout (no se actualizó la hora)");
  }

  // Cortar Wi-Fi para liberar el radio y ahorrar
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  // Volver a ESP-NOW (en STA sin asociar)
  espnow_start();

  return ok;
}

static void ensure_sta_for_scan() {
  // Dejar STA encendida y limpia, pero sin asociar
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true /*apagar/limpiar*/, true /*borrar cred*/);
  delay(50);

  // Asegurar que no quedó nada “raro” del uso previo de ESP-NOW
  esp_wifi_set_promiscuous(false);

  // País: Argentina (canales 1-13). Evita que falten redes en 12/13.
  wifi_country_t AR = { "AR", 1, 13, WIFI_COUNTRY_POLICY_AUTO };
  esp_wifi_set_country(&AR);

  // “Soft restart” del driver (muy efectivo tras ESP-NOW)
  esp_wifi_stop();
  esp_wifi_start();
  delay(50);
}