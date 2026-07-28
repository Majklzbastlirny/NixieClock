// weblog — capture ESP-IDF log output into a RAM ring buffer so it can be read
// back over the web UI (the assembled clock has no serial access anymore).
//
// weblog_init() installs an esp_log vprintf hook that mirrors every ESP_LOGx
// line into a fixed circular buffer (and still forwards to the previous sink,
// so normal console logging is unchanged). The web server dumps the buffer via
// weblog_dump(). Only task-context logs are captured (ISR early-logs bypass the
// hook); that's fine for diagnostics.
#pragma once

#include <stddef.h>

// Install the log hook. Call once, as early in app_main() as possible.
void weblog_init(void);

// Size of the internal ring buffer (upper bound on dump length). Allocate a
// dump buffer of at least weblog_capacity()+1 bytes for the trailing NUL.
size_t weblog_capacity(void);

// Copy the buffered log (oldest->newest, NUL-terminated) into out, at most
// n-1 bytes; if the log is longer than that, the newest n-1 bytes are kept.
// Returns the number of bytes written (excluding the NUL).
size_t weblog_dump(char *out, size_t n);
