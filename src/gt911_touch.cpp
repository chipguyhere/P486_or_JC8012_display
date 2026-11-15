/*
 * GT911 capacitive touch driver for the P486 Panel, on the shared I2C bus.
 * See gt911_touch.h.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#ifdef ARDUINO_ESP32P4_DEV

#include "gt911_touch.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "GT911";

#define GT911_READ_XY_REG       0x814E
#define GT911_CONFIG_REG        0x8047
#define GT911_PRODUCT_ID_REG    0x8140

gt911_touch::gt911_touch(ThreadsafeWire &wire, int8_t rst_pin)
    : _wire(wire), _rst(rst_pin)
{
}

// The GT911's registers are 16-bit, big-endian on the wire, so the 8-bit
// readRegister() helper doesn't fit.  The Guard holds the bus across the
// register write, the repeated start and the drain of Wire's RX buffer.
bool gt911_touch::_read(uint16_t reg, uint8_t *data, size_t len)
{
    ThreadsafeWire::Guard g(_wire);
    if (!g) return false;
    TwoWire &w = _wire.raw();
    w.beginTransmission(_addr);
    w.write((uint8_t)(reg >> 8));
    w.write((uint8_t)(reg & 0xFF));
    if (w.endTransmission(false) != 0) return false;
    if (w.requestFrom(_addr, len) != len) return false;
    for (size_t i = 0; i < len; i++) {
        int b = w.read();
        if (b < 0) return false;
        data[i] = (uint8_t)b;
    }
    return true;
}

bool gt911_touch::_write(uint16_t reg, uint8_t value)
{
    ThreadsafeWire::Guard g(_wire);
    if (!g) return false;
    TwoWire &w = _wire.raw();
    w.beginTransmission(_addr);
    w.write((uint8_t)(reg >> 8));
    w.write((uint8_t)(reg & 0xFF));
    w.write(value);
    return w.endTransmission() == 0;
}

// Read the product ID and config version, as the original driver's init did.
// A successful read is what counts as "the controller is up".
bool gt911_touch::_identify(uint8_t addr)
{
    _addr = addr;
    uint8_t id[3], cfg;
    if (_read(GT911_PRODUCT_ID_REG, id, 3) && _read(GT911_CONFIG_REG, &cfg, 1)) {
        ESP_LOGI(TAG, "TouchPad_ID:0x%02x,0x%02x,0x%02x at 0x%02X, config version %d",
                 id[0], id[1], id[2], addr, cfg);
        return true;
    }
    _addr = 0;
    return false;
}

bool gt911_touch::begin()
{
    _addr = 0;

    // Pulse reset (active low).  After a watchdog reset the GT911 has not been
    // power-cycled and may still be clamping SDA from a half-completed
    // transaction; the reset makes it let go of the bus.
    if (_rst >= 0) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = 1ULL << _rst,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level((gpio_num_t)_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    delay(210);   // the original driver's settle after reset + bus bring-up

    // The GT911 may not respond after a soft reboot due to stale controller
    // state.  Keep retrying until it responds.
    for (int attempt = 1; attempt <= 20; attempt++) {
        if (_identify(ADDR_PRIMARY) || _identify(ADDR_SECONDARY)) return true;
        ESP_LOGW(TAG, "GT911 init attempt %d failed, retrying...", attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGE(TAG, "GT911 init failed after 20 attempts");
    return false;
}

bool gt911_touch::getTouch(uint16_t *x, uint16_t *y)
{
    if (!_addr) return false;

    uint8_t buf[9];
    if (!_read(GT911_READ_XY_REG, buf, 1)) return false;

    // Bit 7: a new frame of coordinates is ready.  Low 4 bits: contacts.
    if ((buf[0] & 0x80) == 0) {
        _write(GT911_READ_XY_REG, 0);
        return false;
    }
    uint8_t n = buf[0] & 0x0F;
    if (n == 0 || n > 5) {
        _write(GT911_READ_XY_REG, 0);
        return false;
    }

    // First contact only: track id, x lo/hi, y lo/hi, size lo/hi, reserved.
    bool ok = _read(GT911_READ_XY_REG + 1, &buf[1], 8);
    _write(GT911_READ_XY_REG, 0);   // release the buffer for the next frame
    if (!ok) return false;

    *x = ((uint16_t)buf[3] << 8) | buf[2];
    *y = ((uint16_t)buf[5] << 8) | buf[4];
    return true;
}

#endif // ARDUINO_ESP32P4_DEV
