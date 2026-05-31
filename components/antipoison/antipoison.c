#include "antipoison.h"
#include "nixie.h"
#include "esp_log.h"

static const char *TAG = "antipoison";

// Tunables (overridable via the setters; web UI/NVS later).
static uint32_t s_period_s     = 600;    // run every 10 min
static uint32_t s_duration_ms  = 12000;  // for 12 s
#define STEP_MS        120               // digit advance interval (~8 steps/s)
#define SCRUB_LEVEL    255               // full brightness so cathodes truly light

static bool    s_active      = false;
static int64_t s_started_ms  = 0;        // when the current run began
static int64_t s_last_run_ms = 0;        // when the last run ended (for period)
static int64_t s_last_step_ms = 0;
static uint8_t s_step        = 0;
static uint8_t s_saved_level = 0;        // clock brightness to restore

static void begin(int64_t now_ms)
{
    s_active = true;
    s_started_ms = now_ms;
    s_last_step_ms = 0;
    s_step = 0;
    s_saved_level = nixie_get_brightness();
    nixie_set_brightness(SCRUB_LEVEL);
    nixie_set_colon(false);
    ESP_LOGI(TAG, "scrub start (%lu ms)", (unsigned long)s_duration_ms);
}

static void end(int64_t now_ms)
{
    s_active = false;
    s_last_run_ms = now_ms;
    nixie_set_brightness(s_saved_level);   // hand brightness back to the clock
    ESP_LOGI(TAG, "scrub done");
}

void antipoison_init(void)
{
    s_last_run_ms = 0;   // first auto-run happens one full period after boot
}

void antipoison_trigger(void)
{
    // Picked up on the next tick; begin() needs a timestamp.
    s_started_ms = 0;
    s_active = true;
    s_step = 0;
    s_last_step_ms = 0;
    s_saved_level = nixie_get_brightness();
    nixie_set_brightness(SCRUB_LEVEL);
    nixie_set_colon(false);
    ESP_LOGI(TAG, "scrub triggered");
}

void antipoison_set_period_s(uint32_t period_s)     { s_period_s = period_s; }
void antipoison_set_duration_ms(uint32_t duration_ms){ s_duration_ms = duration_ms; }

bool antipoison_tick(int64_t now_ms)
{
    if (!s_active) {
        // Time for an automatic run? (period_s == 0 disables auto.)
        if (s_period_s == 0) return false;
        if (now_ms - s_last_run_ms < (int64_t)s_period_s * 1000) return false;
        begin(now_ms);
    } else if (s_started_ms == 0) {
        // Manual trigger from antipoison_trigger(): finish setup now we have time.
        s_started_ms = now_ms;
    }

    if (now_ms - s_started_ms >= (int64_t)s_duration_ms) {
        end(now_ms);
        return false;   // let the caller redraw the clock this very frame
    }

    // Advance the cascade. Each tube is offset by its index, so the digits roll
    // diagonally across the display rather than all showing the same number.
    if (now_ms - s_last_step_ms >= STEP_MS) {
        s_last_step_ms = now_ms;
        uint8_t d[NIXIE_NUM_TUBES];
        for (int i = 0; i < NIXIE_NUM_TUBES; i++)
            d[i] = (uint8_t)((s_step + i) % 10);
        nixie_set_digits(d);
        s_step++;
    }
    return true;
}
