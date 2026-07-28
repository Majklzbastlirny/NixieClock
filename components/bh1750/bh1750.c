#include "bh1750.h"
#include "i2cbus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "bh1750";

#define BH1750_POWER_ON  0x01
#define BH1750_CONT_HRES 0x10   // continuous high-res mode, 1 lx, ~120 ms

static i2c_master_dev_handle_t s_dev;
static bool s_present;

// Try one address: probe, attach a device handle, and kick off continuous mode.
static bool try_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) return false;

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dcfg, &s_dev) != ESP_OK) return false;

    uint8_t cmd = BH1750_POWER_ON;
    i2c_master_transmit(s_dev, &cmd, 1, 100);
    cmd = BH1750_CONT_HRES;
    if (i2c_master_transmit(s_dev, &cmd, 1, 100) != ESP_OK) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "BH1750 found at 0x%02X", addr);
    return true;
}

void bh1750_init(void)
{
    i2c_master_bus_handle_t bus = i2cbus_get();
    if (try_addr(bus, 0x23) || try_addr(bus, 0x5C)) {
        s_present = true;
        vTaskDelay(pdMS_TO_TICKS(180));   // let the first conversion complete
    } else {
        ESP_LOGI(TAG, "no BH1750 on the bus (light-driven dimming unavailable)");
    }
}

bool bh1750_present(void) { return s_present; }

int bh1750_read_lux(void)
{
    if (!s_present || !s_dev) return -1;
    uint8_t raw[2];
    if (i2c_master_receive(s_dev, raw, 2, 100) != ESP_OK) return -1;
    int counts = (raw[0] << 8) | raw[1];
    return (int)(counts / 1.2 + 0.5);   // datasheet: lux = counts / 1.2
}
