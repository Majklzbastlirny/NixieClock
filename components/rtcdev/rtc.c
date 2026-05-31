#include "rtc_clock.h"
#include "i2cbus.h"
#include "esp_log.h"

// Two RTC families are supported, auto-detected at open():
//   - DS1307 / DS3231 @ 0x68: time registers start at 0x00; reg0 bit7 = CH
//     (clock-halt, DS1307 only). Order: s,min,h,wday,day,mon,year.
//   - PCF8563 @ 0x51: time registers start at 0x02; reg2 bit7 = VL (voltage
//     low -> time not guaranteed). Order: s,min,h,day,wday,century_mon,year.
// Both store BCD. We keep to the common fields and run everything in 24h mode.
#define DS_ADDR        0x68
#define DS_REG_SECONDS 0x00
#define DS1307_CH_BIT  0x80   // reg0 bit7: clock-halt (DS1307); 0 on DS3231

#define PCF_ADDR       0x51
#define PCF_REG_SECONDS 0x02
#define PCF_VL_BIT     0x80   // reg2 (seconds) bit7: voltage-low / unreliable

typedef enum { RTC_NONE, RTC_DS, RTC_PCF8563 } rtc_kind_t;

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev = NULL;
static rtc_kind_t s_kind = RTC_NONE;
static bool s_invalid = false;   // last read saw CH (DS) or VL (PCF) set

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static esp_err_t attach(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    return i2c_master_bus_add_device(bus, &dev, &s_dev);
}

esp_err_t rtc_open(void)
{
    i2c_master_bus_handle_t bus = i2cbus_get();

    // Probe PCF8563 first (0x51), then DS-family (0x68).
    if (i2c_master_probe(bus, PCF_ADDR, 100) == ESP_OK) {
        s_kind = RTC_PCF8563;
        ESP_LOGI(TAG, "PCF8563 present at 0x%02X", PCF_ADDR);
        return attach(bus, PCF_ADDR);
    }
    if (i2c_master_probe(bus, DS_ADDR, 100) == ESP_OK) {
        s_kind = RTC_DS;
        ESP_LOGI(TAG, "DS1307/DS3231 present at 0x%02X", DS_ADDR);
        return attach(bus, DS_ADDR);
    }

    s_kind = RTC_NONE;
    ESP_LOGW(TAG, "no RTC found (probed PCF8563@0x51, DS@0x68). Run i2cbus_scan().");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rtc_get(rtc_time_t *out)
{
    if (!s_dev || !out || s_kind == RTC_NONE) return ESP_ERR_INVALID_STATE;

    uint8_t reg = (s_kind == RTC_PCF8563) ? PCF_REG_SECONDS : DS_REG_SECONDS;
    uint8_t b[7];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), 1000);
    if (err != ESP_OK) return err;

    if (s_kind == RTC_PCF8563) {
        // s, min, h, day, wday, century_month, year
        s_invalid    = (b[0] & PCF_VL_BIT) != 0;
        out->second  = bcd2dec(b[0] & 0x7F);
        out->minute  = bcd2dec(b[1] & 0x7F);
        out->hour    = bcd2dec(b[2] & 0x3F);
        out->day     = bcd2dec(b[3] & 0x3F);
        out->weekday = bcd2dec(b[4] & 0x07);
        out->month   = bcd2dec(b[5] & 0x1F);
        out->year    = 2000 + bcd2dec(b[6]);
    } else {
        // s, min, h, wday, day, mon, year
        s_invalid    = (b[0] & DS1307_CH_BIT) != 0;
        out->second  = bcd2dec(b[0] & 0x7F);
        out->minute  = bcd2dec(b[1] & 0x7F);
        out->hour    = bcd2dec(b[2] & 0x3F);   // 24h mode (bit6 = 0)
        out->weekday = bcd2dec(b[3] & 0x07);
        out->day     = bcd2dec(b[4] & 0x3F);
        out->month   = bcd2dec(b[5] & 0x1F);
        out->year    = 2000 + bcd2dec(b[6]);
    }
    return ESP_OK;
}

esp_err_t rtc_set(const rtc_time_t *t)
{
    if (!s_dev || !t || s_kind == RTC_NONE) return ESP_ERR_INVALID_STATE;

    uint8_t buf[8];
    if (s_kind == RTC_PCF8563) {
        buf[0] = PCF_REG_SECONDS;
        buf[1] = dec2bcd((uint8_t)t->second) & 0x7F;   // clears VL -> time valid
        buf[2] = dec2bcd((uint8_t)t->minute) & 0x7F;
        buf[3] = dec2bcd((uint8_t)t->hour) & 0x3F;
        buf[4] = dec2bcd((uint8_t)t->day) & 0x3F;
        buf[5] = dec2bcd((uint8_t)(t->weekday ? t->weekday : 1)) & 0x07;
        buf[6] = dec2bcd((uint8_t)t->month) & 0x1F;    // century bit7 = 0 (20xx)
        buf[7] = dec2bcd((uint8_t)(t->year - 2000));
    } else {
        buf[0] = DS_REG_SECONDS;
        buf[1] = dec2bcd((uint8_t)t->second) & 0x7F;   // CH = 0 -> run
        buf[2] = dec2bcd((uint8_t)t->minute);
        buf[3] = dec2bcd((uint8_t)t->hour) & 0x3F;     // 24h mode
        buf[4] = dec2bcd((uint8_t)(t->weekday ? t->weekday : 1));
        buf[5] = dec2bcd((uint8_t)t->day);
        buf[6] = dec2bcd((uint8_t)t->month);
        buf[7] = dec2bcd((uint8_t)(t->year - 2000));
    }

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
    if (err == ESP_OK) {
        s_invalid = false;
        ESP_LOGI(TAG, "RTC set to %04d-%02d-%02d %02d:%02d:%02d",
                 t->year, t->month, t->day, t->hour, t->minute, t->second);
    }
    return err;
}

bool rtc_present(void)
{
    return s_kind != RTC_NONE;
}

bool rtc_time_valid(const rtc_time_t *t)
{
    if (!t || s_invalid) return false;
    return t->year  >= 2023 && t->year  <= 2099 &&
           t->month >= 1    && t->month <= 12   &&
           t->day   >= 1    && t->day   <= 31   &&
           t->hour  <= 23   && t->minute <= 59  && t->second <= 59;
}
