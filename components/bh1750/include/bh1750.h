// BH1750 ambient-light sensor on the shared I2C bus.
//
// Optional hardware: the clock works fine without it. bh1750_init() probes both
// standard addresses (0x23 with ADDR low, 0x5C with ADDR high) once at boot; if
// a sensor answers it is put in continuous high-resolution mode and its presence
// gates the "lux" night mode in the UI. If nothing answers, every read returns
// -1 and the light-driven night mode is simply not offered.
#pragma once

#include <stdbool.h>

// Probe for a BH1750 at 0x23 then 0x5C and, if found, start continuous H-res
// measurement. Call once at boot, after i2cbus is available.
void bh1750_init(void);

// True if a BH1750 was detected at boot.
bool bh1750_present(void);

// Latest ambient light in lux, or -1 when no sensor is present or a read fails.
int bh1750_read_lux(void);
