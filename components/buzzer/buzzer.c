#include "buzzer.h"
#include "pins.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

#define LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER  LEDC_TIMER_0
#define LEDC_CH     LEDC_CHANNEL_0
#define LEDC_RES    LEDC_TIMER_10_BIT
#define DUTY_ON     512            // ~50% of 1024 = square wave

// Note frequencies for octave 4 (c..b incl. sharps); higher octaves shift up.
static const uint16_t s_note_c4[12] =
    { 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494 };

static char            s_rtttl[256];
static volatile bool   s_loop;
static volatile bool   s_active;
static volatile uint32_t s_gen;     // bumped on every play/stop to interrupt the task
static TaskHandle_t    s_task;

static void tone_on(int freq)
{
    if (freq <= 0) { ledc_set_duty(LEDC_MODE, LEDC_CH, 0); ledc_update_duty(LEDC_MODE, LEDC_CH); return; }
    ledc_set_freq(LEDC_MODE, LEDC_TIMER, freq);
    ledc_set_duty(LEDC_MODE, LEDC_CH, DUTY_ON);
    ledc_update_duty(LEDC_MODE, LEDC_CH);
}

static void tone_off(void)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CH);
}

// Map a note letter (a-g/p) to its index in s_note_c4 (p = -1 pause).
static int note_index(char c)
{
    switch (tolower((unsigned char)c)) {
    case 'c': return 0;
    case 'd': return 2;
    case 'e': return 4;
    case 'f': return 5;
    case 'g': return 7;
    case 'a': return 9;
    case 'b': return 11;
    default:  return -1;   // 'p' pause or unknown
    }
}

// Play s_rtttl once (or until s_gen changes). Returns false if interrupted.
static bool play_once(uint32_t my_gen)
{
    const char *p = s_rtttl;
    const char *colon = strchr(p, ':');
    if (!colon) return true;

    // --- defaults section (between first and second colon) ---
    int d_def = 4, o_def = 6, bpm = 63;
    const char *defs = colon + 1;
    const char *colon2 = strchr(defs, ':');
    if (!colon2) return true;
    for (const char *q = defs; q < colon2; q++) {
        if (q[0] == 'd' && q[1] == '=') d_def = atoi(q + 2);
        else if (q[0] == 'o' && q[1] == '=') o_def = atoi(q + 2);
        else if (q[0] == 'b' && q[1] == '=') bpm = atoi(q + 2);
    }
    if (bpm <= 0) bpm = 63;
    int whole_ms = (240000 / bpm);   // whole note = 4 beats

    // --- notes section ---
    p = colon2 + 1;
    while (*p) {
        if (s_gen != my_gen) return false;

        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;

        int dur = 0;
        while (isdigit((unsigned char)*p)) dur = dur * 10 + (*p++ - '0');
        if (dur == 0) dur = d_def;

        char letter = *p ? *p++ : 0;
        int idx = note_index(letter);
        if (*p == '#') { if (idx >= 0) idx++; p++; }

        int oct = o_def;
        bool dotted = false;
        if (*p == '.') { dotted = true; p++; }
        if (isdigit((unsigned char)*p)) oct = *p++ - '0';
        if (*p == '.') { dotted = true; p++; }

        int ms = whole_ms / dur;
        if (dotted) ms += ms / 2;

        if (idx < 0) {
            tone_off();
        } else {
            int freq = s_note_c4[idx];
            if (oct >= 4) freq <<= (oct - 4); else freq >>= (4 - oct);
            tone_on(freq);
        }

        // Hold the note, then a short gap so repeated notes are distinct.
        int hold = ms * 9 / 10, gap = ms - hold;
        vTaskDelay(pdMS_TO_TICKS(hold));
        tone_off();
        if (gap > 0) vTaskDelay(pdMS_TO_TICKS(gap));
    }
    return true;
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        uint32_t my_gen = s_gen;
        bool finished = play_once(my_gen);
        if (s_gen != my_gen) continue;        // replaced/stopped: re-evaluate
        if (finished && !s_loop) {            // natural end of a one-shot
            s_active = false;
            tone_off();
        }
        // looping: fall through and play again
    }
}

void buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CH,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
    tone_off();

    xTaskCreate(player_task, "buzzer", 3072, NULL, 5, &s_task);
    ESP_LOGI(TAG, "init on GPIO%d", PIN_BUZZER);
}

void buzzer_play_rtttl(const char *rtttl, bool loop)
{
    if (!rtttl) return;
    strlcpy(s_rtttl, rtttl, sizeof(s_rtttl));
    s_loop = loop;
    s_active = true;
    s_gen++;       // interrupt any current playback; task picks up the new one
}

void buzzer_stop(void)
{
    s_active = false;
    s_loop = false;
    s_gen++;
    tone_off();
}

bool buzzer_is_playing(void)
{
    return s_active;
}

// A handful of short RTTTL melodies for the alarm.
static const char *s_melodies[BUZZER_MELODY_COUNT] = {
    "Beep:d=4,o=5,b=120:8c6,8p,8c6,8p,8c6,8p,2p",
    "Reveille:d=4,o=5,b=140:8g,8c6,8e6,8g6,8e6,8c6,8g,8c6,8e6,8g6,8e6,8c6",
    "Westminster:d=4,o=5,b=90:e,g,f,c,p,c,f,g,e,p",
    "Scale:d=8,o=5,b=160:c,d,e,f,g,a,b,c6,p,c6,b,a,g,f,e,d,c",
    "Siren:d=8,o=5,b=120:c6,g,c6,g,c6,g,c6,g",
    "Nokia:d=4,o=5,b=125:8e6,8d6,f#,g#,8c#6,8b,d,e,8b,8a,c#,e,2a",
    "Axelf:d=4,o=5,b=125:g,8a#.,16g,16p,16g,8c6,8g,8f,g,8d.6,16g,16p,16g,8d#6,8d6,8a#,8g,8d6,8g6",
    "Triad:d=4,o=5,b=100:c,e,g,c6,p,c6,g,e,c",
};

const char *buzzer_builtin_rtttl(int index)
{
    if (index < 0) index = 0;
    return s_melodies[index % BUZZER_MELODY_COUNT];
}
