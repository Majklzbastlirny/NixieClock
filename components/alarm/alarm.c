#include "alarm.h"
#include "buzzer.h"

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
