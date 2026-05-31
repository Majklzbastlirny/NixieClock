#include "buttons.h"
#include "pins.h"
#include "nixie.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "buttons";

// --- Tube-index -> action map (BOARD-SPECIFIC, set after hardware discovery) -
// The 5 multiplexed buttons sit on tube slots 1..5 (slot 0 has no button) —
// confirmed on hardware 2026-05-31 via the discovery log. Index 0 = leftmost tube.
#define IDX_BRIGHT_UP    1
#define IDX_BRIGHT_DOWN  2
#define IDX_SHOW_TEMP    3
#define IDX_ANTIPOISON   4
#define IDX_PROVISION    5

#define DISCOVER_LOG     0     // 1 = log every multiplexed press index

// --- Timing -----------------------------------------------------------------
#define DEBOUNCE_MS       30
#define SHORT_MAX_MS     1500  // <= this on release = "short press"
#define PROVISION_MS     3000  // hold to enter provisioning
#define FACTORY_MS      10000  // hold to factory-reset
#define SNOOZE_LONG_MS   1000  // snooze hold = dismiss
#define COMBO_MS         2000  // bright+ & bright- held together = alarm toggle

#define NUM_MUX  NIXIE_NUM_TUBES   // 6 multiplexed slots
#define SNOOZE_I NUM_MUX           // logical index for the dedicated button
#define NUM_BTN  (NUM_MUX + 1)

typedef struct {
    bool    raw;             // last raw sample
    bool    stable;          // debounced state (true = pressed)
    int64_t last_change_ms;  // when raw last changed (for debounce)
    int64_t press_ms;        // when the debounced press began
} btn_t;

static btn_t        s_btn[NUM_BTN];
static button_cb_t  s_cb;
static int64_t      s_combo_start;   // when bright+ & bright- first both pressed (0 = not)
static bool         s_combo_fired;   // emitted the toggle for this combo hold

static void emit(button_event_t ev)
{
    if (s_cb) s_cb(ev);
}

void buttons_init(button_cb_t cb)
{
    s_cb = cb;

    // The multiplexed KEY line is configured by the nixie driver. Only the
    // dedicated snooze button needs setup here (active-low, internal pull-up).
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_SNOOZE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    for (int i = 0; i < NUM_BTN; i++) {
        s_btn[i].raw = s_btn[i].stable = false;
        s_btn[i].last_change_ms = 0;
        s_btn[i].press_ms = 0;
    }
    ESP_LOGI(TAG, "init (discover_log=%d)", DISCOVER_LOG);
}

// A multiplexed slot just registered a debounced press: route to its action.
// IDX_PROVISION is intentionally handled on release (by hold duration), not here.
static void on_mux_press(int idx)
{
    switch (idx) {
    case IDX_BRIGHT_UP:   emit(BTN_BRIGHT_UP);   break;
    case IDX_BRIGHT_DOWN: emit(BTN_BRIGHT_DOWN); break;
    case IDX_SHOW_TEMP:   emit(BTN_SHOW_TEMP);   break;
    case IDX_ANTIPOISON:  emit(BTN_ANTIPOISON);  break;
    default: break;   // IDX_PROVISION handled on release (hold duration)
    }
}

static bool read_raw(int i)
{
    if (i == SNOOZE_I)
        return gpio_get_level(PIN_SNOOZE) == 0;   // active low
    return nixie_key_pressed(i);                   // already per-slot sampled
}

void buttons_tick(int64_t now_ms)
{
    for (int i = 0; i < NUM_BTN; i++) {
        btn_t *b = &s_btn[i];
        bool raw = read_raw(i);

        if (raw != b->raw) {            // raw edge: restart debounce window
            b->raw = raw;
            b->last_change_ms = now_ms;
            continue;
        }
        if (now_ms - b->last_change_ms < DEBOUNCE_MS) continue;
        if (raw == b->stable) continue; // no committed change

        // Committed state change.
        b->stable = raw;
        if (raw) {
            // --- press edge ---
            b->press_ms = now_ms;
#if DISCOVER_LOG
            if (i < NUM_MUX) ESP_LOGI(TAG, "KEY press at tube index %d", i);
            else             ESP_LOGI(TAG, "SNOOZE press");
#endif
            if (i < NUM_MUX && i != IDX_PROVISION)
                on_mux_press(i);
        } else {
            // --- release edge: classify holds ---
            int64_t held = now_ms - b->press_ms;
            if (i == IDX_PROVISION) {
                if (held >= FACTORY_MS)        emit(BTN_FACTORY_RESET);
                else if (held >= PROVISION_MS) emit(BTN_PROVISION);
                else                           emit(BTN_SHOW_IP);   // short tap
#if DISCOVER_LOG
                ESP_LOGI(TAG, "provision button held %lldms", (long long)held);
#endif
            } else if (i == SNOOZE_I) {
                emit(held >= SNOOZE_LONG_MS ? BTN_SNOOZE_LONG : BTN_SNOOZE_SHORT);
            }
        }
    }

    // Combo: bright+ and bright- held together for COMBO_MS toggles the alarm.
    // (Each fired one brightness step on its press edge; equal +/- nets ~zero.)
    if (s_btn[IDX_BRIGHT_UP].stable && s_btn[IDX_BRIGHT_DOWN].stable) {
        if (s_combo_start == 0) s_combo_start = now_ms;
        if (!s_combo_fired && now_ms - s_combo_start >= COMBO_MS) {
            s_combo_fired = true;
            emit(BTN_ALARM_TOGGLE);
        }
    } else {
        s_combo_start = 0;
        s_combo_fired = false;
    }
}
