#include "antipoison.h"
#include "nixie.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "antipoison";

// Tunables, pushed from settings by main (apply_antipoison).
static uint32_t s_period_s     = 600;    // run every 10 min
static uint32_t s_duration_ms  = 12000;  // for 12 s
static uint32_t s_step_ms      = 120;    // digit advance interval (~8 steps/s)
static uint8_t  s_anim         = AP_ANIM_CASCADE;
#define SCRUB_LEVEL    255               // full brightness so cathodes truly light

static bool    s_active      = false;
static int64_t s_started_ms  = 0;        // when the current run began
static int64_t s_last_run_ms = 0;        // when the last run ended (for period)
static int64_t s_last_step_ms = 0;
static uint32_t s_step       = 0;
static uint8_t s_saved_level = 0;        // clock brightness to restore
static volatile bool s_trigger_req = false; // manual run requested (any task)
static volatile bool s_waiting = false;  // auto run due but held for an interlude

static uint8_t s_run_anim = AP_ANIM_CASCADE;  // what this run draws (mix resolved)
static uint8_t s_mix_next = 0;                // mix: next animation to use
static uint8_t s_reel_off[NIXIE_NUM_TUBES];   // reels: per-tube start digit

static const char *const ANIM_NAMES[AP_ANIM_COUNT] = {
    "cascade", "sync", "random", "reels", "wave", "mix",
};

const char *antipoison_anim_name(uint8_t anim)
{
    return anim < AP_ANIM_COUNT ? ANIM_NAMES[anim] : ANIM_NAMES[AP_ANIM_CASCADE];
}

uint8_t antipoison_anim_from_name(const char *name)
{
    for (uint8_t i = 0; i < AP_ANIM_COUNT; i++)
        if (strcasecmp(name, ANIM_NAMES[i]) == 0) return i;
    return AP_ANIM_CASCADE;
}

static void begin(int64_t now_ms)
{
    if (!s_active)   // a restart mid-run must not "save" the scrub level
        s_saved_level = nixie_get_brightness();
    s_active = true;
    s_waiting = false;
    s_started_ms = now_ms;
    s_last_step_ms = 0;
    s_step = 0;

    s_run_anim = s_anim;
    if (s_anim == AP_ANIM_MIX) {                 // rotate through the real ones
        s_run_anim = s_mix_next;
        s_mix_next = (uint8_t)((s_mix_next + 1) % AP_ANIM_MIX);
    }
    for (int i = 0; i < NIXIE_NUM_TUBES; i++)
        s_reel_off[i] = (uint8_t)(esp_random() % 10);

    nixie_set_brightness(SCRUB_LEVEL);
    nixie_set_colon(false);
    ESP_LOGI(TAG, "scrub start: %s, %lu ms, %lu ms/step", antipoison_anim_name(s_run_anim),
             (unsigned long)s_duration_ms, (unsigned long)s_step_ms);
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
    // Also called from the web UI / MQTT tasks, so only raise a request here:
    // antipoison_tick() starts the run in the main loop, keeping the routine's
    // state and the tube brightness owned by a single task.
    s_trigger_req = true;
    ESP_LOGI(TAG, "scrub triggered");
}

void antipoison_set_period_s(uint32_t period_s)      { s_period_s = period_s; }
void antipoison_set_duration_ms(uint32_t duration_ms) { s_duration_ms = duration_ms; }
void antipoison_set_step_ms(uint32_t step_ms)         { s_step_ms = step_ms ? step_ms : 120; }
void antipoison_set_anim(uint8_t anim)                { s_anim = anim < AP_ANIM_COUNT ? anim : AP_ANIM_CASCADE; }

// One frame of the current animation. Every one of them walks each tube through
// all ten digits evenly over time, which is the whole point of the scrub.
static void draw_step(uint32_t step)
{
    uint8_t d[NIXIE_NUM_TUBES];
    for (int i = 0; i < NIXIE_NUM_TUBES; i++) {
        switch (s_run_anim) {
        case AP_ANIM_SYNC:
            d[i] = (uint8_t)(step % 10);
            break;
        case AP_ANIM_RANDOM:
            d[i] = (uint8_t)(esp_random() % 10);
            break;
        case AP_ANIM_REELS:   // tube i turns every 1, 2 or 3 steps from a random start
            d[i] = (uint8_t)((s_reel_off[i] + step / (uint32_t)(1 + i % 3)) % 10);
            break;
        case AP_ANIM_WAVE: {  // centre tubes lead, the edges follow
            int dist = abs(2 * i - (NIXIE_NUM_TUBES - 1)) / 2;
            d[i] = (uint8_t)((step + 10 - (uint32_t)dist % 10) % 10);
            break;
        }
        case AP_ANIM_CASCADE:
        default:              // each tube offset by its index: a diagonal roll
            d[i] = (uint8_t)((step + (uint32_t)i) % 10);
            break;
        }
    }
    nixie_set_digits(d);
}

bool antipoison_tick(int64_t now_ms, bool may_start)
{
    if (s_trigger_req) {
        // Manual trigger from antipoison_trigger(): (re)start the run now.
        s_trigger_req = false;
        begin(now_ms);
    } else if (!s_active) {
        // Time for an automatic run? (period_s == 0 disables auto.)
        if (s_period_s == 0) { s_waiting = false; return false; }
        if (now_ms - s_last_run_ms < (int64_t)s_period_s * 1000) return false;
        if (!may_start) {
            if (!s_waiting) ESP_LOGI(TAG, "scrub due, waiting for a gap between interludes");
            s_waiting = true;
            return false;
        }
        begin(now_ms);
    }

    if (now_ms - s_started_ms >= (int64_t)s_duration_ms) {
        end(now_ms);
        return false;   // let the caller redraw the clock this very frame
    }

    if (s_last_step_ms == 0 || now_ms - s_last_step_ms >= (int64_t)s_step_ms) {
        s_last_step_ms = now_ms;
        draw_step(s_step++);
    }
    return true;
}

void antipoison_status_str(char *buf, size_t n)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_active)          { snprintf(buf, n, "running"); return; }
    if (s_period_s == 0)   { snprintf(buf, n, "off"); return; }
    if (s_waiting)         { snprintf(buf, n, "waiting for a gap"); return; }
    int64_t left = (int64_t)s_period_s * 1000 - (now - s_last_run_ms);
    if (left < 0) left = 0;
    int m = (int)(left / 60000), s = (int)(left / 1000 % 60);
    if (m) snprintf(buf, n, "in %dm%02ds", m, s);
    else   snprintf(buf, n, "in %ds", s);
}
