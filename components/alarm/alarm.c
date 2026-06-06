#include "alarm.h"
#include "buzzer.h"
#include "settings.h"

#include <stdio.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "alarm";

typedef enum { ST_IDLE, ST_RINGING, ST_SNOOZED } state_t;

static state_t  s_state = ST_IDLE;
static int64_t  s_snooze_until_ms;
static uint8_t  s_melody;            // melody index in use this ring
static uint8_t  s_snooze_min;        // snapshot for re-arming

// Guards so we trigger once per occurrence and stay quiet after dismiss:
//   s_fired_key    = yday*1440 + minute we already started ringing for
//   s_dismiss_yday = day-of-year we dismissed on (suppress until it changes)
static int s_fired_key    = -1;
static int s_dismiss_yday = -1;

void alarm_init(void)
{
    s_state = ST_IDLE;
}

static bool dow_matches(uint8_t mask, int tm_wday)
{
    return mask == 0 || (mask & (1u << tm_wday));   // bit0=Sun..bit6=Sat; 0=daily
}

static void start_ringing(uint8_t melody)
{
    s_melody = melody;
    s_state = ST_RINGING;
    buzzer_play_rtttl(buzzer_builtin_rtttl(melody), true);
    ESP_LOGI(TAG, "RINGING (melody %u)", melody);
}

void alarm_tick(const struct tm *lt, const clock_settings_t *c, int64_t now_ms)
{
    int mins = lt->tm_hour * 60 + lt->tm_min;
    int key  = lt->tm_yday * 1440 + mins;

    // A new day clears the "dismissed today" suppression.
    if (lt->tm_yday != s_dismiss_yday) s_dismiss_yday = -1;

    if (!c->alarm_enabled) {
        if (s_state != ST_IDLE) { buzzer_stop(); s_state = ST_IDLE; }
        return;
    }

    s_snooze_min = c->alarm_snooze_min ? c->alarm_snooze_min : 9;

    switch (s_state) {
    case ST_IDLE: {
        bool time_match = (lt->tm_hour == c->alarm_hour && lt->tm_min == c->alarm_min);
        if (time_match && dow_matches(c->alarm_dow_mask, lt->tm_wday) &&
            key != s_fired_key && lt->tm_yday != s_dismiss_yday) {
            s_fired_key = key;          // this occurrence is handled
            start_ringing(c->alarm_melody);
        }
        break;
    }
    case ST_SNOOZED:
        if (now_ms >= s_snooze_until_ms)
            start_ringing(s_melody);
        break;
    case ST_RINGING:
        // Safety: if the buzzer task somehow stopped, keep it looping.
        if (!buzzer_is_playing())
            buzzer_play_rtttl(buzzer_builtin_rtttl(s_melody), true);
        break;
    }
}

bool alarm_is_ringing(void) { return s_state == ST_RINGING; }

bool alarm_is_snoozed(void) { return s_state == ST_SNOOZED; }

bool alarm_is_armed(void)
{
    // "Armed" = not dismissed-for-today. main also checks alarm_enabled itself
    // before using this for the colon cue.
    return s_dismiss_yday == -1;
}

void alarm_snooze(void)
{
    if (s_state != ST_RINGING) return;
    buzzer_stop();
    s_snooze_until_ms = (esp_timer_get_time() / 1000) + (int64_t)s_snooze_min * 60 * 1000;
    s_state = ST_SNOOZED;
    ESP_LOGI(TAG, "snoozed %u min", s_snooze_min);
}

bool alarm_dismiss(void)
{
    if (s_state == ST_IDLE) return false;
    buzzer_stop();
    s_state = ST_IDLE;
    // Suppress further rings until tomorrow (reuse the day we last fired on).
    s_dismiss_yday = s_fired_key / 1440;
    ESP_LOGI(TAG, "dismissed for today");
    return true;
}

// Minutes from `lt` until the next alarm occurrence, honouring the day-of-week
// mask. `skip_today` ignores today's slot (used when already dismissed). Returns
// -1 if no day matches (shouldn't happen while enabled).
static int mins_to_next_alarm(const clock_settings_t *c, const struct tm *lt,
                              bool skip_today)
{
    int now_min   = lt->tm_hour * 60 + lt->tm_min;
    int alarm_min = c->alarm_hour * 60 + c->alarm_min;
    for (int d = 0; d <= 7; d++) {
        int wday = (lt->tm_wday + d) % 7;
        if (!dow_matches(c->alarm_dow_mask, wday)) continue;
        if (d == 0) {
            if (skip_today || alarm_min <= now_min) continue;  // already gone today
            return alarm_min - now_min;
        }
        return d * 1440 - now_min + alarm_min;
    }
    return -1;
}

static void fmt_dur(int mins, char *o, size_t n)
{
    if (mins < 60) snprintf(o, n, "%dm", mins);
    else           snprintf(o, n, "%dh%02dm", mins / 60, mins % 60);
}

void alarm_status_str(const clock_settings_t *c, const struct tm *lt,
                      char *out, size_t n)
{
    if (!c->alarm_enabled) { snprintf(out, n, "disabled"); return; }
    if (s_state == ST_RINGING) { snprintf(out, n, "ringing"); return; }

    if (s_state == ST_SNOOZED) {
        int64_t now = esp_timer_get_time() / 1000;
        int rem = (int)((s_snooze_until_ms - now + 59999) / 60000);  // round up
        if (rem < 0) rem = 0;
        snprintf(out, n, "snoozed %dm", rem);
        return;
    }

    // IDLE: either dismissed for the rest of today, or simply armed.
    bool dismissed = (s_dismiss_yday != -1);
    int  m = mins_to_next_alarm(c, lt, dismissed);
    char d[16];
    if (m >= 0) fmt_dur(m, d, sizeof(d)); else snprintf(d, sizeof(d), "?");
    snprintf(out, n, "%s %s", dismissed ? "off" : "armed", d);
}

void alarm_preview_melody(void)
{
    clock_settings_t c;
    settings_get(&c);
    buzzer_play_rtttl(buzzer_builtin_rtttl(c.alarm_melody), false);
    ESP_LOGI(TAG, "preview melody %u", c.alarm_melody);
}
