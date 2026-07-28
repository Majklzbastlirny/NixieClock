#include "weblog.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

// Ring buffer of recent log text. Sized to hold a useful scrollback (~a few
// hundred lines) without eating much RAM. Access is guarded by a portMUX: the
// writer (put) runs from arbitrary logging tasks, the reader (dump) from the
// httpd task. Sections are short (memcpy only), so ISR jitter is negligible.
#define LOG_BUF_SZ 6144
#define WEBLOG_LINE_MAX    256

static char             s_buf[LOG_BUF_SZ];
static size_t           s_head;      // next write index
static bool             s_wrapped;   // buffer has filled at least once
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t   s_prev;      // previous log sink (console), chained

static void put(const char *p, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = p[i];
        if (s_head >= LOG_BUF_SZ) { s_head = 0; s_wrapped = true; }
    }
    portEXIT_CRITICAL(&s_mux);
}

static int log_hook(const char *fmt, va_list ap)
{
    // Format once for the ring buffer; va_copy so the chained sink still works.
    va_list ap2;
    va_copy(ap2, ap);
    char line[WEBLOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) put(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));

    int r = s_prev ? s_prev(fmt, ap2) : n;
    va_end(ap2);
    return r;
}

void weblog_init(void)
{
    s_prev = esp_log_set_vprintf(log_hook);
}

size_t weblog_capacity(void)
{
    return LOG_BUF_SZ;
}

size_t weblog_dump(char *out, size_t n)
{
    if (n < 2) { if (n) out[0] = '\0'; return 0; }
    const size_t cap = n - 1;

    portENTER_CRITICAL(&s_mux);
    size_t total = s_wrapped ? LOG_BUF_SZ : s_head;
    size_t want  = total < cap ? total : cap;
    size_t skip  = total - want;                 // drop oldest when truncating
    size_t len   = 0;

    if (s_wrapped) {
        size_t start = s_head + skip;            // logical 0 == s_head
        if (start >= LOG_BUF_SZ) start -= LOG_BUF_SZ;
        size_t first = LOG_BUF_SZ - start;
        if (first > want) first = want;
        memcpy(out, s_buf + start, first);
        len = first;
        if (want > first) {
            memcpy(out + len, s_buf, want - first);
            len += want - first;
        }
    } else {
        memcpy(out, s_buf + skip, want);
        len = want;
    }
    portEXIT_CRITICAL(&s_mux);

    out[len] = '\0';
    return len;
}
