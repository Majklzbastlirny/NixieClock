// Shared I2C master bus (ESP-IDF 5.x i2c_master driver).
//
// One bus, created once, handed out to every on-bus device so they don't fight
// over the controller. Right now that's just the RTC @ 0x68; the future BH1750
// light sensor @ 0x23 will attach to the same handle.
#pragma once

#include "driver/i2c_master.h"

// Create (idempotent) and return the shared master bus on the clock's I2C pins.
// Safe to call from multiple init paths; subsequent calls return the same bus.
i2c_master_bus_handle_t i2cbus_get(void);

// Probe every 7-bit address (0x08..0x77) and log which ones ACK. Handy when a
// new device (RTC, sensor) doesn't show up and you need to find its address.
// Returns the number of devices that responded.
int i2cbus_scan(void);
