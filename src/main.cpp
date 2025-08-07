#include <lvgl.h>
#include <TFT_eSPI.h>
#include "aqs_images.h"
#include <WiFi.h>
#include <DHT.h>
#include <time.h>
#include <XPT2046_Touchscreen.h>

// Replace with your network credentials
const char* ssid = "Fibertel WiFi867 2.4GHz";
const char* password = "0043559168";

// Enter your location
String location = "Bahia Blanca";

// Store date and time
String temperature;
String humidity;
String monoxide;

// Define the pin for the buzzer
#define BUZZER_PIN 15

// SET VARIABLE TO 0 FOR TEMPERATURE IN FAHRENHEIT DEGREES
#define TEMP_CELSIUS 1

#if TEMP_CELSIUS
  String temperature_unit = "";
  const char degree_symbol[] = "\u00B0C";
#else
  String temperature_unit = "&temperature_unit=fahrenheit";
  const char degree_symbol[] = "\u00B0F";
#endif

// DHT sensor setup (if needed, not used in this example)
#define DHTPIN 22           // Cambiá esto al pin que uses
#define DHTTYPE DHT22

// Initialize DHT sensor
DHT dht(DHTPIN, DHTTYPE);

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

static bool alert_blink_state = false;
static bool alert_active = false;

void setup() {
  String LVGL_Arduino = String("LVGL Library Version: ") + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
  Serial.begin(115200);
  Serial.println(LVGL_Arduino);

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // buzzer apagado al inicio

  // Initialize DHT sensor
  dht.begin();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nConnected to Wi-Fi network with IP Address: ");
  Serial.println(WiFi.localIP());

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

  // Create and show the splash screen
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
  LV_IMAGE_DECLARE(image_settings);

  get_weather_data();

  // ---------- FONDO CON BORDE Y SOMBRA VERDE ----------
  screen_bg = lv_obj_create(lv_screen_active());
  lv_obj_set_size(screen_bg, lv_obj_get_width(lv_screen_active()), lv_obj_get_height(lv_screen_active()));
  lv_obj_center(screen_bg);
  lv_obj_set_style_radius(screen_bg, 0, 0);
  lv_obj_set_style_bg_opa(screen_bg, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(screen_bg, 4, 0);
  lv_obj_set_style_border_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
  //lv_obj_set_style_shadow_width(screen_bg, 15, 0);
  //lv_obj_set_style_shadow_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
  //lv_obj_set_style_shadow_spread(screen_bg, 0, 0);

  // ---------- ÍCONO DE ESTADO ----------
  image_status_icon = lv_image_create(lv_screen_active());
  lv_image_set_src(image_status_icon, &image_cleanair);
  lv_obj_align(image_status_icon, LV_ALIGN_CENTER, -80, -20);

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
  lv_timer_t * timer = lv_timer_create(timer_cb, 30000, NULL);
  lv_timer_ready(timer);

  // Timer para hacer parpadear el borde si hay alerta
  lv_timer_create(alert_blink_cb, 500, NULL);  // cada 500 ms
}

// Function to get weather data from the DHT sensor
void get_weather_data() {
  float t = dht.readTemperature();   // Lee temperatura en °C
  float h = dht.readHumidity();      // Lee humedad en %
  float m = 15.0; // Simulación de valor de monóxido de carbono (MQ7) en ppm

  if (isnan(t) || isnan(h)) {
    Serial.println("Error al leer del sensor DHT22");
    return;
  }

  temperature = String(t, 1);  // Un decimal
  humidity = String(h, 1);
  monoxide = String(m, 1); // Un decimal
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
  lv_label_set_text(text_label_time_location, (get_formatted_datetime() + " | " + location).c_str());

    // Verificar nivel de CO
  float ppm = monoxide.toFloat();
  if (ppm > 100.0) {
    alert_active = true;
    lv_image_set_src(image_status_icon, &image_alert);  // cambiar icono

  } else {
    if (alert_active) {
      // solo si veníamos de alerta, restauramos
      lv_image_set_src(image_status_icon, &image_cleanair);
      lv_obj_set_style_border_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_set_style_shadow_color(screen_bg, lv_palette_main(LV_PALETTE_GREEN), 0);
    }
      alert_active = false;
      alert_blink_state = false;
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
  digitalWrite(BUZZER_PIN, alert_blink_state ? HIGH : LOW);
}

// Get the Touchscreen data
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  if (touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();

    // Modo horizontal con rotación 270°
    // Intercambiamos ejes y los invertimos para que coincidan
    int x = map(p.y, 3902, 316, 0, SCREEN_WIDTH);   // p.y → X pantalla (derecha a izquierda)
    int y = map(p.x, 3742, 315, 0, SCREEN_HEIGHT);  // p.x → Y pantalla (abajo a arriba)

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;

    Serial.print("X = ");
    Serial.print(x);
    Serial.print(" | Y = ");
    Serial.println(y);
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void lv_create_splash_screen() {
  LV_IMAGE_DECLARE(image_init);

  splash_screen = lv_screen_active();
  lv_obj_t * img = lv_image_create(splash_screen);
  lv_image_set_src(img, &image_init);
  lv_obj_align(img, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t * label = lv_label_create(splash_screen);
  lv_label_set_text(label, "Air Quality System");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 60);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  splash_timer = lv_timer_create([](lv_timer_t * timer) {
    lv_obj_clean(lv_screen_active());
    lv_create_main_gui();
    lv_timer_del(timer);
  }, 3000, NULL);
}

void lv_create_config_menu() {
  config_screen = lv_obj_create(NULL);
  lv_obj_set_size(config_screen, SCREEN_WIDTH, SCREEN_HEIGHT);

  lv_obj_t * label = lv_label_create(config_screen);
  lv_label_set_text(label, "Men\u00fa de configuraci\u00f3n");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 40);

  // Botón "Volver"
  lv_obj_t * btn_back = lv_btn_create(config_screen);
  lv_obj_set_size(btn_back, 80, 40);  // ← Tamaño fijo necesario
  lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 10, -10);

  lv_obj_t * label_btn = lv_label_create(btn_back);
  lv_label_set_text(label_btn, "Volver");

  lv_obj_add_event_cb(btn_back, [](lv_event_t * e) {
    lv_scr_load(main_screen);
  }, LV_EVENT_CLICKED, NULL);

  lv_scr_load(config_screen);
}