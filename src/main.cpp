#include <lvgl.h>
#include <TFT_eSPI.h>
#include "aqs_images.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <time.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include <esp_now.h>

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

// === Contexto para nombrar sensor ===
struct NameCtx {
  lv_obj_t * ta;   // textarea donde se escribe el nombre
  size_t     idx;  // índice del sensor en 'sensors'
};
static NameCtx g_name_ctx;

static std::vector<DiscoveredSensor> sensors;
static int selectedSensorIndex = -1;     // índice del sensor activo (en 'sensors'), -1 = ninguno
static const uint32_t SENSOR_STALE_MS = 30000; // visible en listas si visto en últimos 15s

// Label para mostrar el nombre del sensor activo
static lv_obj_t * text_label_sensor_name = nullptr;

// ====== Estado de recepción ======
static volatile bool espnow_has_data = false;
static uint32_t last_rx_ms = 0;
static const uint32_t ESPNOW_TIMEOUT_MS = 15000; // 15s: si no llegan datos, usamos fallback

// Vector to store available Wi-Fi SSIDs
std::vector<String> availableSSIDs;
String wifi_ssid = "";
String wifi_password = "";
static const int WIFI_LIST_LIMIT = 10;

// Enter your location
String location = "Bahia Blanca";

// Store date and time
String temperature;
String humidity;
String monoxide;

// Touchscreen coordinates: (x, y) and pressure (z)
int x, y;

// Define the pin for the buzzer
#define BUZZER_PIN 15
static bool buzzer_muted = false;

// SET VARIABLE TO 0 FOR TEMPERATURE IN FAHRENHEIT DEGREES
#define TEMP_CELSIUS 1

#if TEMP_CELSIUS
  String temperature_unit = "";
  const char degree_symbol[] = "\u00B0C";
#else
  String temperature_unit = "&temperature_unit=fahrenheit";
  const char degree_symbol[] = "\u00B0F";
#endif

// Touchscreen pins
#define XPT2046_IRQ 27   // T_IRQ
#define XPT2046_MOSI 13  // T_DIN
#define XPT2046_MISO 12  // T_OUT
#define XPT2046_CLK 14   // T_CLK
#define XPT2046_CS 33    // T_CS

SPIClass touchscreenSPI(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// Screen dimensions
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 480

#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

// Forward declarations
void get_weather_description(int code);
void get_weather_data();
void log_print(lv_log_level_t level, const char * buf);
static void timer_cb(lv_timer_t * timer);
void lv_create_main_gui(void);
String get_formatted_datetime();
static void alert_blink_cb(lv_timer_t * timer);
void touchscreen_event_cb(lv_event_t * e);
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data);
void lv_create_splash_screen();
void lv_create_config_menu();
void scan_and_show_wifi_list(lv_obj_t * parent);
void show_wifi_keyboard(const char * ssid);
void connect_to_wifi(String ssid, String password);
void lv_create_config_menu();
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
static void sensor_back_btn_cb(lv_event_t * e);
static lv_obj_t * build_wifi_style_keyboard(lv_obj_t * parent, lv_obj_t * textarea);

static void refresh_ui_now();
static void set_selected_sensor(int idx);
static void select_next_sensor();
static void select_prev_sensor();
static void update_nav_arrows();
static void auto_rotate_cb(lv_timer_t *t);
static void ensure_auto_rotate_timer();

static lv_obj_t * text_label_temperature;
static lv_obj_t * text_label_humidity;
static lv_obj_t * text_label_time_location;
static lv_obj_t * text_label_ppm;
static lv_obj_t * screen_bg;
static lv_obj_t * image_status_icon;  // Ícono dinámico: cleanair o alert
static lv_obj_t * splash_screen;  // pantalla temporal
static lv_obj_t * main_screen;
static lv_obj_t * config_screen;
static lv_timer_t * splash_timer;
static lv_obj_t * kb;
static lv_obj_t * ta;  // Text area para contraseña
static String selected_ssid = "";
static lv_style_t style_btn_close;
static lv_style_t style_btn_ok;
static bool alert_blink_state = false;
static bool alert_active = false;

// --- Navegación de sensores en pantalla única ---
static lv_obj_t *btn_prev = nullptr;
static lv_obj_t *btn_next = nullptr;
static uint32_t last_user_nav_ms = 0;     // pausa autorrotación tras interacción
#define AUTO_ROTATE_MS 0                  // 0 = off. Ej: 6000 para rotar cada 6s

static lv_timer_t *auto_rotate_timer = nullptr;

// ====== Teclado: Shift (↑) mayúsculas/minúsculas ======
static bool kb_caps = true;  // true: mayúsculas, false: minúsculas

// Mapa MAYÚSCULAS (Shift reemplaza al Enter debajo de Backspace)
static const char * KB_MAP_UPPER[] = {
  "1","2","3","4","5","6","7","8","9","0","\n",
  "Q","W","E","R","T","Y","U","I","O","P", LV_SYMBOL_BACKSPACE, "\n",
  "A","S","D","F","G","H","J","K","L",     LV_SYMBOL_UP,        "\n", // <<-- Shift aquí
  "Z","X","C","V","B","N","M",",",".","!","?","\n",
  LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, NULL
};

// Mapa MINÚSCULAS
static const char * KB_MAP_LOWER[] = {
  "1","2","3","4","5","6","7","8","9","0","\n",
  "q","w","e","r","t","y","u","i","o","p", LV_SYMBOL_BACKSPACE, "\n",
  "a","s","d","f","g","h","j","k","l",     LV_SYMBOL_UP,        "\n", // <<-- Shift aquí
  "z","x","c","v","b","n","m",",",".","!","?","\n",
  LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, NULL
};

// Controles/ancho de las teclas (igual que usabas, con Shift ocupando el ancho del viejo Enter)
static const lv_buttonmatrix_ctrl_t KB_CTRL_MAP[] = {
  // Fila 1: 1–0 (10)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,

  // Fila 2: Q–P (10) + Backspace (1)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,   // backspace más ancho

  // Fila 3: A–L (9) + Shift (1)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,   // Shift ancho (ocupa el lugar del Enter)

  // Fila 4: Z–? (11)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,

  // Fila 5: Close, Space, OK (3)
  LV_BUTTONMATRIX_CTRL_WIDTH_3,   // Close
  LV_BUTTONMATRIX_CTRL_WIDTH_6,   // Space
  LV_BUTTONMATRIX_CTRL_WIDTH_3,   // OK
};

void setup() {
  String LVGL_Arduino = String("LVGL Library Version: ") + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
  Serial.begin(115200);
  Serial.println(LVGL_Arduino);

  // --- ESP-NOW ---
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inicializando ESP-NOW");
  } else {
    esp_now_register_recv_cb(on_espnow_recv);
    Serial.printf("ESP-NOW listo. Canal actual: %d\n", wifi_channel());
  }

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // buzzer apagado al inicio

  // Connect to Wi-Fi
  Serial.println("Esperando conexión WiFi desde menú.");

  configTzTime("GMT+3", "pool.ntp.org", "time.nist.gov");

  // Start LVGL
  lv_init();
  // Register print function for debugging
  lv_log_register_print_cb(log_print);

  // Start the SPI for the touchscreen and init the touchscreen
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  // Create a display object
  lv_display_t * disp;
  // Initialize the TFT display using the TFT_eSPI library
  disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

  // Initialize an LVGL input device object (Touchscreen)
  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  // Set the callback function to read Touchscreen input
  lv_indev_set_read_cb(indev, touchscreen_read);

  // Pantalla de inicio y pantalla principal
  lv_create_splash_screen();
  main_screen = lv_screen_active();
}

void loop() {
  lv_task_handler();  // let the GUI do its work
  lv_tick_inc(5);     // tell LVGL how much time has passed
  delay(5);           // let this time pass
}

void lv_create_main_gui(void) {

  LV_IMAGE_DECLARE(image_weather_temperature);
  LV_IMAGE_DECLARE(image_weather_humidity);
  LV_IMAGE_DECLARE(image_monoxide);
  LV_IMAGE_DECLARE(image_cleanair);
  LV_IMAGE_DECLARE(image_alert);
  LV_IMAGE_DECLARE(image_warning);
  LV_IMAGE_DECLARE(image_settings);

  get_weather_data();

  // ---------- FONDO CON BORDE Y SOMBRA VERDE ----------
  screen_bg = lv_obj_create(lv_screen_active());
  lv_obj_set_size(screen_bg, lv_obj_get_width(lv_screen_active()), lv_obj_get_height(lv_screen_active()));
  lv_obj_center(screen_bg);
  lv_obj_set_style_radius(screen_bg, 0, 0);
  lv_obj_set_style_bg_opa(screen_bg, LV_OPA_TRANSP, 0);

  // Nombre del sensor activo (arriba centrado)
  text_label_sensor_name = lv_label_create(lv_screen_active());
  lv_label_set_text(text_label_sensor_name, "Sin sensor");
  lv_obj_align(text_label_sensor_name, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_font(text_label_sensor_name, &lv_font_montserrat_22, 0);

  // ---------- ÍCONO DE ESTADO ----------
  image_status_icon = lv_image_create(lv_screen_active());
  lv_image_set_src(image_status_icon, &image_cleanair);
  lv_obj_align(image_status_icon, LV_ALIGN_CENTER, -100, -10);

  // Temperature Icon
  lv_obj_t * weather_image_temperature = lv_image_create(lv_screen_active());
  lv_image_set_src(weather_image_temperature, &image_weather_temperature);
  lv_obj_align(weather_image_temperature, LV_ALIGN_CENTER, 30, -60);

  text_label_temperature = lv_label_create(lv_screen_active());
  lv_label_set_text(text_label_temperature, String("      " + temperature + degree_symbol).c_str());
  lv_obj_align(text_label_temperature, LV_ALIGN_CENTER, 95, -60);
  lv_obj_set_style_text_font((lv_obj_t*) text_label_temperature, &lv_font_montserrat_22, 0);

  // Humidity Icon
  lv_obj_t * weather_image_humidity = lv_image_create(lv_screen_active());
  lv_image_set_src(weather_image_humidity, &image_weather_humidity);
  lv_obj_align(weather_image_humidity, LV_ALIGN_CENTER, 30, 15);

  text_label_humidity = lv_label_create(lv_screen_active());
  lv_label_set_text(text_label_humidity, String("   " + humidity + "%").c_str());
  lv_obj_align(text_label_humidity, LV_ALIGN_CENTER, 95, 15);
  lv_obj_set_style_text_font((lv_obj_t*) text_label_humidity, &lv_font_montserrat_22, 0);

  // Monoxide Icon
  lv_obj_t * icon_monoxide = lv_image_create(lv_screen_active());
  lv_image_set_src(icon_monoxide, &image_monoxide);  // reemplazá con tu imagen
  lv_obj_align(icon_monoxide, LV_ALIGN_CENTER, 30, 80);

  text_label_ppm = lv_label_create(lv_screen_active());
  lv_label_set_text(text_label_ppm, String("   " + monoxide + " ppm").c_str());  // valor fijo por ahora
  lv_obj_align(text_label_ppm, LV_ALIGN_CENTER, 120, 80);
  lv_obj_set_style_text_font((lv_obj_t*) text_label_ppm, &lv_font_montserrat_22, 0);

  // ---------- BOTÓN DE CONFIGURACIÓN ----------
  lv_obj_t * btn_settings = lv_image_create(lv_screen_active());
  lv_image_set_src(btn_settings, &image_settings);
  lv_obj_align(btn_settings, LV_ALIGN_BOTTOM_LEFT, 10, -10);  // esquina inferior izquierda
  lv_obj_add_flag(btn_settings, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_settings, [](lv_event_t * e) {
    lv_create_config_menu();
  }, LV_EVENT_CLICKED, NULL);

  // Create a text label for the time and timezone aligned center in the bottom of the screen
  text_label_time_location = lv_label_create(lv_screen_active());
  String datetime_str = get_formatted_datetime() + " | " + location;
  lv_label_set_text(text_label_time_location, datetime_str.c_str());
  lv_obj_align(text_label_time_location, LV_ALIGN_BOTTOM_MID, 0, -10);

  lv_obj_set_style_text_font(text_label_time_location, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(text_label_time_location, lv_palette_main(LV_PALETTE_GREY), 0);

  // Create a timer to update the weather data every 30 seconds
  lv_timer_t * timer = lv_timer_create(timer_cb, 5000, NULL);
  lv_timer_ready(timer);

  // Timer para hacer parpadear el borde si hay alerta
  lv_timer_create(alert_blink_cb, 500, NULL);  // cada 500 ms

  update_nav_arrows();       // crea flechas y ajusta visibilidad
  ensure_auto_rotate_timer(); // si AUTO_ROTATE_MS > 0
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

static void timer_cb(lv_timer_t * timer){
  LV_UNUSED(timer);
  get_weather_data();
  
  lv_label_set_text(text_label_temperature, String("      " + temperature + degree_symbol).c_str());
  lv_label_set_text(text_label_humidity, String("   " + humidity + "%").c_str());
  lv_label_set_text(text_label_ppm, String("   " + monoxide + " ppm").c_str());
  lv_label_set_text(text_label_time_location, (get_formatted_datetime() + " | " + location).c_str());

    // Verificar nivel de CO
  float ppm = monoxide.toFloat();
  
  if (ppm > 100.0) {
    // si se acaba de entrar en alerta, reactivamos el buzzer (quitamos mute)
    if (!alert_active) {
      buzzer_muted = false;
    }
    alert_active = true;
    lv_image_set_src(image_status_icon, &image_alert);
  } else {
    if (alert_active) {
      // veníamos de alerta y se normalizó
      lv_image_set_src(image_status_icon, &image_cleanair);
      lv_obj_set_style_border_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_set_style_shadow_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
    }
    alert_active = false;
    alert_blink_state = false;
    buzzer_muted = false;
    digitalWrite(BUZZER_PIN, LOW);
  }
   update_nav_arrows();       // por si aparece el 2º sensor “en caliente”
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

  if (!alert_active) {
    // Si no hay alerta, apagamos el buzzer y salimos
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  alert_blink_state = !alert_blink_state;

  lv_color_t color = alert_blink_state ? lv_palette_main(LV_PALETTE_RED) : lv_color_black();
  lv_obj_set_style_border_color(screen_bg, color, 0);
  lv_obj_set_style_shadow_color(screen_bg, color, 0);

  // Activar o desactivar el buzzer
  //digitalWrite(BUZZER_PIN, alert_blink_state ? HIGH : LOW);

  if (!buzzer_muted) {
    digitalWrite(BUZZER_PIN, alert_blink_state ? HIGH : LOW);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// Get the Touchscreen data
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  if(touchscreen.tirqTouched() && touchscreen.touched()) {
    // Get Touchscreen points
    TS_Point p = touchscreen.getPoint();

    // Advanced Touchscreen calibration, LEARN MORE » https://RandomNerdTutorials.com/touchscreen-calibration/
    float alpha_x, beta_x, alpha_y, beta_y, delta_x, delta_y;

    // REPLACE WITH YOUR OWN CALIBRATION VALUES » https://RandomNerdTutorials.com/touchscreen-calibration/
    alpha_x = 0.001;
    beta_x = -0.130;
    delta_x = 498.426;
    alpha_y = -0.087;
    beta_y = 0.001;
    delta_y = 339.434;

    x = alpha_y * p.x + beta_y * p.y + delta_y;
    // clamp x between 0 and SCREEN_WIDTH - 1
    x = max(0, x);
    x = min(SCREEN_WIDTH - 1, x);

    y = alpha_x * p.x + beta_x * p.y + delta_x;
    // clamp y between 0 and SCREEN_HEIGHT - 1
    y = max(0, y);
    y = min(SCREEN_HEIGHT - 1, y);

    data->state = LV_INDEV_STATE_PRESSED;

    // Set the coordinates
    data->point.x = x;
    data->point.y = y;

    // Si hay alerta, cualquier toque silencia el buzzer
    if (alert_active) {
      buzzer_muted = true;
      digitalWrite(BUZZER_PIN, LOW);
    }

    // Print Touchscreen info about X, Y and Pressure (Z) on the Serial Monitor
    Serial.print("X = ");
    Serial.print(x);
    Serial.print(" | Y = ");
    Serial.print(y);
    Serial.println();
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
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
    lv_timer_del(timer);
  }, 1000, NULL);
}

void scan_and_show_wifi_list(lv_obj_t * parent, int max_items) {
  availableSSIDs.clear();

  int n = WiFi.scanNetworks();  // sincrónico
  if (n <= 0) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "No se encontraron redes WiFi.");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 0);
    return;
  }

  // Estilo para texto más grande
  static lv_style_t style_wifi_text;
  static bool style_wifi_text_inited = false;
  if(!style_wifi_text_inited) {
      style_wifi_text_inited = true;
      lv_style_init(&style_wifi_text);
      lv_style_set_text_font(&style_wifi_text, &lv_font_montserrat_18); // ajusta el tamaño aquí
  }

  int count = min(n, max_items);
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    availableSSIDs.push_back(ssid);

    // Botón por red
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_width(btn, lv_pct(70));
    lv_obj_set_height(btn, 30);
    //lv_obj_center(btn);

    lv_obj_t * label = lv_label_create(btn);
    lv_label_set_text(label, ssid.c_str());
    lv_obj_center(label);
    lv_obj_add_style(label, &style_wifi_text, 0); // aplicar estilo de fuente

    lv_obj_update_layout(parent);          // recalcula layout
    lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);  // fuerza scroll al tope

    // Evento: al tocar, abrir teclado de contraseña
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
      lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
      lv_obj_t * label = lv_obj_get_child(btn, 0);
      const char * ssid_selected = lv_label_get_text(label);
      show_wifi_keyboard(ssid_selected);
    }, LV_EVENT_CLICKED, NULL);
  }
  
  lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);
}

void show_wifi_keyboard(const char * ssid) {
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
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());

  lv_obj_clean(lv_screen_active());

  lv_obj_t * label = lv_label_create(lv_screen_active());
  lv_label_set_text(label, "Conectando...");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Conectado a WiFi");
    lv_label_set_text(label, "¡Conectado!");
    delay(2000);
    lv_scr_load(main_screen);  // Volver a la pantalla principal
  } else {
    lv_label_set_text(label, "Error al conectar");
    delay(2000);
    lv_obj_clean(lv_screen_active());
    lv_create_config_menu();  // Volver al menú de configuración
  }
}

void lv_create_config_menu() {
  LV_IMAGE_DECLARE(image_back);

  static lv_style_t style_btn_text;
  static bool style_btn_text_inited = false;
  if(!style_btn_text_inited) {
      style_btn_text_inited = true;
      lv_style_init(&style_btn_text);
      lv_style_set_text_font(&style_btn_text, &lv_font_montserrat_28); // fuente más grande
  }

  // Crear pantalla de configuración
  config_screen = lv_obj_create(NULL);
  lv_obj_set_size(config_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(config_screen);

  // Título
  lv_obj_t * title = lv_label_create(config_screen);
  lv_label_set_text(title, "Menu de configuracion");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver
  lv_obj_t * btn_back = lv_image_create(config_screen);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, [](lv_event_t * e) {
    lv_scr_load(main_screen);
  }, LV_EVENT_CLICKED, NULL);

  // Contenedor para opciones (columna)
  lv_obj_t * list = lv_obj_create(config_screen);
  lv_obj_set_size(list, lv_pct(90), lv_pct(65));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 12, 0);
  lv_obj_set_style_pad_all(list, 10, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  // Quitar borde, sombra y contorno
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
  lv_obj_add_style(lbl_wifi, &style_btn_text, 0); // aplicar estilo de fuente
  lv_obj_add_event_cb(btn_wifi, [](lv_event_t * e) {
    lv_create_wifi_menu();
  }, LV_EVENT_CLICKED, NULL);

  // Botón Agregar Sensor (placeholder)
  lv_obj_t * btn_sensor = lv_btn_create(list);
  lv_obj_set_width(btn_sensor, lv_pct(100));
  lv_obj_set_height(btn_sensor, 50);
  lv_obj_t * lbl_sensor = lv_label_create(btn_sensor);
  lv_label_set_text(lbl_sensor, "Agregar sensor");
  lv_obj_center(lbl_sensor);
  lv_obj_add_style(lbl_sensor, &style_btn_text, 0); // aplicar estilo de fuente
  lv_obj_add_event_cb(btn_sensor, [](lv_event_t * e) {
  open_sensor_list_screen();
  }, LV_EVENT_CLICKED, NULL);
}

void lv_create_wifi_menu() {
  LV_IMAGE_DECLARE(image_back);

  lv_obj_t * wifi_screen = lv_obj_create(NULL);
  lv_obj_set_size(wifi_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(wifi_screen);

  // Título
  lv_obj_t * title = lv_label_create(wifi_screen);
  lv_label_set_text(title, "Redes WiFi");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver
  lv_obj_t * btn_back = lv_image_create(wifi_screen);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, [](lv_event_t * e) {
    lv_create_config_menu();
  }, LV_EVENT_CLICKED, NULL);

  // Contenedor scrollable para la lista
  lv_obj_t * list = lv_obj_create(wifi_screen);
  lv_obj_set_size(list, lv_pct(90), lv_pct(70));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);


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

  // Etiqueta "Escaneando..."
  lv_obj_t * scanning = lv_label_create(list);
  lv_label_set_text(scanning, "Escaneando...");
  lv_obj_center(scanning);

  // Hacemos el escaneo (sincrónico) y mostramos máx. 10
  // (si querés, podés cambiar a WiFi.scanNetworks(true) y esperar, pero así es simple)
  lv_timer_t * t = lv_timer_create_basic();
  lv_timer_set_period(t, 10);
  lv_timer_set_repeat_count(t, 1);
  lv_timer_set_user_data(t, list);
  lv_timer_set_cb(t, [](lv_timer_t * t) {
    lv_obj_t * parent_list = (lv_obj_t *)lv_timer_get_user_data(t);
    lv_obj_clean(parent_list); // limpiar "Escaneando..."
    scan_and_show_wifi_list(parent_list, WIFI_LIST_LIMIT);
  });
}

static void on_espnow_recv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (!mac || len != sizeof(struct_message)) return;

  // Registrar/actualizar sensor visto
  int idx = touch_or_add_sensor(mac);

  // Guardar último paquete
  memcpy(&sensors[idx].last, incomingData, sizeof(struct_message));
  sensors[idx].lastSeen = millis();

  // Log útil
  const struct_message &in = sensors[idx].last;
  Serial.printf("[ESP-NOW] RX %s | S:%c T:%.1f H:%.1f CO:%.1f (named:'%s')\n",
                sensors[idx].macStr.c_str(), in.sensor, in.temp, in.float_hum, in.mono,
                sensors[idx].name.c_str());

  // ACK opcional si el emisor lo pide
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
  
  if (selectedSensorIndex < 0) selectedSensorIndex = idx;
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
  // Cerrar pantalla previa (si existía) y matar timer previo
  if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
  if (sensor_list_screen) { lv_obj_del(sensor_list_screen); sensor_list_screen = nullptr; }

  sensor_list_screen = lv_obj_create(NULL);
  lv_obj_set_size(sensor_list_screen, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_scr_load(sensor_list_screen);

  // Título
  lv_obj_t * title = lv_label_create(sensor_list_screen);
  lv_label_set_text(title, "Sensores disponibles");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // Botón Volver (usa tu ícono 'image_back' si querés)
  LV_IMAGE_DECLARE(image_back);
  lv_obj_t * btn_back = lv_image_create(sensor_list_screen);
  lv_image_set_src(btn_back, &image_back);
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(btn_back, sensor_back_btn_cb, LV_EVENT_CLICKED, NULL);

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

  int shown = 0;
  uint32_t now = millis();

  for (size_t i = 0; i < sensors.size(); ++i) {
    if (now - sensors[i].lastSeen > SENSOR_STALE_MS) continue; // muy viejo: no mostrar

    lv_obj_t * btn = lv_btn_create(sensor_list_container);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 40);

    String line = (sensors[i].name.length() ? sensors[i].name : sensors[i].macStr);
    line += "   ";
    line += String(sensors[i].last.temp,1) + "°C  ";
    line += String(sensors[i].last.float_hum,1) + "%  ";
    line += String(sensors[i].last.mono,0) + "ppm";

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, line.c_str());
    lv_obj_center(lbl);

    // Guardar índice como user_data y conectar callback
    lv_obj_set_user_data(btn, (void*)i);
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
    size_t idx = (size_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    open_name_screen(idx); // <-- usa la pantalla con teclado estilo Wi‑Fi
    }, LV_EVENT_CLICKED, NULL);

    shown++;
  }

  if (shown == 0) {
    // Mensaje de búsqueda continua
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

// Botón volver: regresar al menú de configuración
static void sensor_back_btn_cb(lv_event_t * e) {
  LV_UNUSED(e);
  if (sensor_scan_timer) { lv_timer_del(sensor_scan_timer); sensor_scan_timer = nullptr; }
  if (sensor_list_screen) { lv_obj_del(sensor_list_screen); sensor_list_screen = nullptr; }
  sensor_list_container = nullptr;
  lv_create_config_menu();
}

// ---- Teclado estilo Wi‑Fi (Shift ↑ NO escribe en el textarea) ----
// ---- Teclado estilo Wi‑Fi (Shift ↑ NO se escribe porque el textarea lo rechaza) ----
static lv_obj_t * build_wifi_style_keyboard(lv_obj_t * parent, lv_obj_t * textarea) {
    lv_obj_t * kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, SCREEN_HEIGHT, SCREEN_WIDTH / 2);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Estado inicial: mayúsculas
    kb_caps = true;
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, KB_MAP_UPPER, KB_CTRL_MAP);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);

    // Vincular textarea
    lv_keyboard_set_textarea(kb, textarea);

    // === Punto CLAVE: limitar caracteres aceptados por el textarea ===
    // Armá la lista a tu gusto; acá va algo amplio para contraseñas.
    static const char * ALLOWED =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        " .,;-_!?#@$/\\\"'()[]{}=+*<>|%&:^~`";  // incluye espacio
    lv_textarea_set_accepted_chars(textarea, ALLOWED);
    // =================================================================

    // Estilo
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

    // Alternar mayúsculas/minúsculas al tocar ↑
    lv_obj_add_event_cb(kb, [](lv_event_t * e){
        if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

        lv_obj_t * kb_ = (lv_obj_t *)lv_event_get_target(e);
        uint16_t id = lv_btnmatrix_get_selected_btn(kb_);
        if (id == LV_BTNMATRIX_BTN_NONE) return;

        const char * txt = lv_btnmatrix_get_btn_text(kb_, id);
        if (!txt) return;

        if (strcmp(txt, LV_SYMBOL_UP) == 0 || strcmp(txt, "Shift") == 0) {
            kb_caps = !kb_caps;
            if (kb_caps) {
                lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_USER_1, KB_MAP_UPPER, KB_CTRL_MAP);
            } else {
                lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_USER_1, KB_MAP_LOWER, KB_CTRL_MAP);
            }
            // No hace falta cortar eventos; el textarea ya no aceptará ↑
            return;
        }
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

static void update_nav_arrows() {
  // Crear si no existen (en la pantalla activa — tu principal)
  if (!btn_prev) {
    btn_prev = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn_prev, 36, 36);
    lv_obj_align(btn_prev, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_add_event_cb(btn_prev, [](lv_event_t *){
      last_user_nav_ms = millis();
      select_prev_sensor();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_prev);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_center(lbl);
  }

  if (!btn_next) {
    btn_next = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn_next, 36, 36);
    lv_obj_align(btn_next, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_add_event_cb(btn_next, [](lv_event_t *){
      last_user_nav_ms = millis();
      select_next_sensor();
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_next);
    lv_label_set_text(lbl, LV_SYMBOL_RIGHT);
    lv_obj_center(lbl);
  }

  // Mostrar/ocultar según cantidad de sensores
  bool show = sensors.size() >= 2;
  if (btn_prev) (show ? lv_obj_clear_flag(btn_prev, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_HIDDEN));
  if (btn_next) (show ? lv_obj_clear_flag(btn_next, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(btn_next, LV_OBJ_FLAG_HIDDEN));

  // Si aún no hay seleccionado y sí hay sensores, elegí el primero
  if (selectedSensorIndex < 0 && sensors.size() > 0) {
    set_selected_sensor(0);
  }
}

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
