#include "i2cbus.h"
#include "pins.h"
#include "esp_log.h"

static const char *TAG = "i2cbus";
static i2c_master_bus_handle_t s_bus = NULL;

i2c_master_bus_handle_t i2cbus_get(void)
{
    if (s_bus) return s_bus;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // board has its own pullups too
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    ESP_LOGI(TAG, "I2C master bus up on SDA=%d SCL=%d", PIN_I2C_SDA, PIN_I2C_SCL);
    return s_bus;
}

int i2cbus_scan(void)
{
    i2c_master_bus_handle_t bus = i2cbus_get();
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan complete: %d device(s)", found);
    return found;
}
