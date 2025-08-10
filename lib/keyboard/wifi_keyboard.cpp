#include "wifi_keyboard.h"
#include <string.h>

// --- Estado Shift (mayúsculas/minúsculas) ---
static bool kb_caps = true;

// --- Mapas de teclas ---
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

// Controles/ancho de tecla (igual que tu versión)
static const lv_buttonmatrix_ctrl_t KB_CTRL_MAP[] = {
  // Fila 1: 1–0 (10)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,

  // Fila 2: Q–P (10) + Backspace (1)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,

  // Fila 3: A–L (9) + Shift (1)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_2,

  // Fila 4: Z–? (11)
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
  LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,

  // Fila 5: Close, Space, OK
  LV_BUTTONMATRIX_CTRL_WIDTH_3, LV_BUTTONMATRIX_CTRL_WIDTH_6, LV_BUTTONMATRIX_CTRL_WIDTH_3
};

// --- Callback sin lambdas: alterna MAY/min al tocar ↑ ---
static void kb_value_changed_cb(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;

  lv_obj_t * kb = (lv_obj_t *)lv_event_get_target(e);
  // API v9:
  uint16_t id = lv_buttonmatrix_get_selected_button(kb);
  if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;

  const char * txt = lv_buttonmatrix_get_button_text(kb, id);
  if (!txt) return;

  if (strcmp(txt, LV_SYMBOL_UP) == 0 || strcmp(txt, "Shift") == 0) {
    kb_caps = !kb_caps;
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1,
                        kb_caps ? KB_MAP_UPPER : KB_MAP_LOWER,
                        KB_CTRL_MAP);
  }
}

lv_obj_t* build_wifi_style_keyboard(lv_obj_t* parent, lv_obj_t* textarea) {
  lv_obj_t * kb = lv_keyboard_create(parent);
  lv_obj_set_size(kb, lv_pct(100), lv_pct(45));
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

  // Arrancamos en mayúsculas
  kb_caps = true;
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, KB_MAP_UPPER, KB_CTRL_MAP);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);

  // Vincular textarea
  lv_keyboard_set_textarea(kb, textarea);

  // Limitar caracteres aceptados (opcional)
  static const char * ALLOWED =
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789"
      " .,;-_!?#@$/\\\"'()[]{}=+*<>|%&:^~`";
  lv_textarea_set_accepted_chars(textarea, ALLOWED);

  // Estilo simple
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

  // Registrar callback SIN lambdas
  lv_obj_add_event_cb(kb, kb_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  return kb;
}