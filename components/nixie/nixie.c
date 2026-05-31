#include "nixie.h"

#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"

// --- Refresh timing ------------------------------------------------------
// One tube gets SLOT_US of the cycle. 6 tubes * 1500us = 9ms/frame ~= 111 Hz,
// well above flicker. Each slot has a LIT phase (anode on, length set by
// brightness) followed by a DEAD-TIME phase that is >= MIN_BLANK_US.
//
// Anti-ghosting: the six tubes share the cathode lines (74141); only the anode
// is per-tube (74x42). So two rules matter, both handled in the ISR below:
//   1. Set the cathode digit BEFORE enabling the tube's anode, never after —
//      otherwise the new tube briefly lights the *previous* digit while the
//      cathodes are still changing (a fixed-length glitch that's most visible
//      when dim, i.e. the "faint other digits while faded" symptom).
//   2. During dead-time, blank ALL anodes (select code 6/7 = display off on
//      this board) so the gas de-ionises before the next tube fires.
#define SLOT_US        1500
#define MIN_ON_US       120   // shortest lit time that still ionises the tube
#define MIN_BLANK_US    250   // dead-time floor (de-ionise + slow-MH74141 settle)
#define SELECT_OFF        7   // 74x42 code with no tube wired => all anodes off

// BCD code that produces no cathode output on the 74141 (input > 9 = all off).
#define BCD_BLANK      0x0F

// --- Shared state (ISR <-> API) ------------------------------------------
static volatile uint8_t s_digit[NIXIE_NUM_TUBES];
static volatile bool    s_dot[NIXIE_NUM_TUBES];
static volatile uint8_t s_level   = 180;          // brightness 0..255
static volatile int     s_on_us   = 0;            // derived from s_level
static volatile uint8_t s_key_bits = 0xFF;        // 1 = released, 0 = pressed (per tube)

static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static gptimer_handle_t s_timer;

// --- Low-level pin writes (ISR context, must stay in IRAM) ---------------
static inline void IRAM_ATTR write_select(uint8_t code)
{
    gpio_set_level(PIN_SEL0, (code >> 0) & 1);
    gpio_set_level(PIN_SEL1, (code >> 1) & 1);
    gpio_set_level(PIN_SEL2, (code >> 2) & 1);
}

static inline void IRAM_ATTR write_bcd(uint8_t v)
{
    gpio_set_level(PIN_BCD0, (v >> 0) & 1);
    gpio_set_level(PIN_BCD1, (v >> 1) & 1);
    gpio_set_level(PIN_BCD2, (v >> 2) & 1);
    gpio_set_level(PIN_BCD3, (v >> 3) & 1);
}

// Board select code is reversed vs. our left-to-right framebuffer index.
static inline uint8_t IRAM_ATTR sel_code(int idx)
{
    return (uint8_t)((NIXIE_NUM_TUBES - 1) - idx);
}

static inline void IRAM_ATTR sample_key(int idx)
{
    if (gpio_get_level(PIN_KEY))
        s_key_bits |= (uint8_t)(1u << idx);    // released
    else
        s_key_bits &= (uint8_t)~(1u << idx);   // pressed (active low)
}

// --- Refresh state machine -----------------------------------------------
// Two events per tube slot:
//   LIT  : set cathode, then enable this tube's anode (order matters, see top),
//          sample its multiplexed button, hold for on_us.
//   DEAD : blank all anodes + cathode, hold for the rest of the slot so the gas
//          de-ionises before the next tube lights.
static bool IRAM_ATTR on_alarm(gptimer_handle_t timer,
                               const gptimer_alarm_event_data_t *edata,
                               void *user_ctx)
{
    (void)user_ctx;
    static int  tube = 0;
    static bool dead_phase = false;   // start by lighting tube 0

    uint64_t next = edata->alarm_value;
    int on_us = s_on_us;

    if (!dead_phase) {
        // --- LIT phase for `tube` ---
        uint8_t d = s_digit[tube];
        bool show = (d <= 9) && (on_us > 0);

        // Cathode FIRST (stable digit), then bring the anode up — never reverse.
        write_bcd(show ? d : BCD_BLANK);
        gpio_set_level(PIN_DOT, (show && s_dot[tube]) ? 1 : 0);
        write_select(sel_code(tube));     // KEY mux now points at this tube too
        sample_key(tube);

        // Even at brightness 0 we briefly select the tube (cathode blanked, so
        // it stays dark) so its button still gets scanned.
        next += (on_us > 0) ? on_us : MIN_ON_US;
        dead_phase = true;
    } else {
        // --- DEAD-TIME phase ---
        write_select(SELECT_OFF);         // all anodes off
        write_bcd(BCD_BLANK);
        gpio_set_level(PIN_DOT, 0);

        int blank = SLOT_US - on_us;
        if (blank < MIN_BLANK_US) blank = MIN_BLANK_US;
        next += blank;

        tube = (tube + 1) % NIXIE_NUM_TUBES;
        dead_phase = false;
    }

    gptimer_alarm_config_t alarm = {
        .alarm_count = next,
        .flags.auto_reload_on_alarm = false,
    };
    gptimer_set_alarm_action(timer, &alarm);
    return false;
}

// --- Brightness math -----------------------------------------------------
static void recompute_timing(void)
{
    int level = s_level;
    int on_us;
    if (level == 0) {
        on_us = 0;
    } else {
        const int span = SLOT_US - MIN_ON_US - MIN_BLANK_US;   // usable range
        on_us = MIN_ON_US + (span * level) / 255;
        if (on_us > SLOT_US - MIN_BLANK_US)
            on_us = SLOT_US - MIN_BLANK_US;                    // keep blank tail
    }
    portENTER_CRITICAL(&s_mux);
    s_on_us = on_us;
    portEXIT_CRITICAL(&s_mux);
}

// --- GPIO setup ----------------------------------------------------------
static void configure_gpio(void)
{
    const uint64_t out_mask =
        (1ULL << PIN_SEL0) | (1ULL << PIN_SEL1) | (1ULL << PIN_SEL2) |
        (1ULL << PIN_BCD0) | (1ULL << PIN_BCD1) | (1ULL << PIN_BCD2) | (1ULL << PIN_BCD3) |
        (1ULL << PIN_DOT)  | (1ULL << PIN_COLON);

    gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << PIN_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // KEY is open-drain active-low
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key_cfg);

    // Start fully blanked.
    write_bcd(BCD_BLANK);
    write_select(SELECT_OFF);
    gpio_set_level(PIN_DOT, 0);
    gpio_set_level(PIN_COLON, 0);
}

void nixie_init(void)
{
    for (int i = 0; i < NIXIE_NUM_TUBES; i++) {
        s_digit[i] = NIXIE_BLANK;
        s_dot[i] = false;
    }
    configure_gpio();
    recompute_timing();

    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,   // 1 tick = 1 us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &s_timer));

    gptimer_event_callbacks_t cbs = { .on_alarm = on_alarm };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));

    gptimer_alarm_config_t alarm = {
        .alarm_count = 100,        // first event 100us after start
        .flags.auto_reload_on_alarm = false,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &alarm));
    ESP_ERROR_CHECK(gptimer_start(s_timer));
}

// --- Public framebuffer API ----------------------------------------------
void nixie_set_digit(int idx, uint8_t value)
{
    if (idx < 0 || idx >= NIXIE_NUM_TUBES) return;
    s_digit[idx] = (value <= 9) ? value : NIXIE_BLANK;
}

void nixie_set_digits(const uint8_t values[NIXIE_NUM_TUBES])
{
    for (int i = 0; i < NIXIE_NUM_TUBES; i++)
        s_digit[i] = (values[i] <= 9) ? values[i] : NIXIE_BLANK;
}

void nixie_set_dot(int idx, bool on)
{
    if (idx < 0 || idx >= NIXIE_NUM_TUBES) return;
    s_dot[idx] = on;
}

void nixie_set_colon(bool on)
{
    gpio_set_level(PIN_COLON, on ? 1 : 0);
}

void nixie_set_brightness(uint8_t level)
{
    s_level = level;
    recompute_timing();
}

uint8_t nixie_get_brightness(void)
{
    return s_level;
}

bool nixie_key_pressed(int idx)
{
    if (idx < 0 || idx >= NIXIE_NUM_TUBES) return false;
    return (s_key_bits & (1u << idx)) == 0;
}
