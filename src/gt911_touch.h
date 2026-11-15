/*
 * GT911 capacitive touch driver for the P486 Panel, on the shared I2C bus.
 *
 * Adapted from chipguy_P486Panel_touch, which opened its own ESP-IDF I2C master
 * bus on port 0.  That cannot coexist with this library's ThreadsafeWire, which
 * already owns port 0 on GPIO7/8 (it is how the board is identified at boot),
 * so the register access here goes through the shared bus instead.  The
 * reset, retry and read sequence are unchanged.
 *
 * Reports the first contact in NATIVE panel coordinates (720x720, no mirroring).
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

#include <Arduino.h>
#include "chipguy_ThreadsafeWire.h"

class gt911_touch {
public:
    // The GT911 latches one of two addresses at reset depending on its INT
    // line, which this board leaves unconnected.  begin() tries both.
    static constexpr uint8_t ADDR_PRIMARY   = 0x5D;
    static constexpr uint8_t ADDR_SECONDARY = 0x14;

    gt911_touch(ThreadsafeWire &wire, int8_t rst_pin);

    // Pulses reset, then retries until the controller answers (20 x 250 ms):
    // after a soft reboot the GT911 can take a while to respond.  Returns false
    // if it never does; getTouch() then reports no touches.
    bool begin();

    // First contact, in native 720x720 coordinates.  False when not touched.
    bool getTouch(uint16_t *x, uint16_t *y);

    bool ok() const { return _addr != 0; }
    uint8_t address() const { return _addr; }

private:
    ThreadsafeWire &_wire;
    int8_t  _rst;
    uint8_t _addr = 0;

    bool _read(uint16_t reg, uint8_t *data, size_t len);
    bool _write(uint16_t reg, uint8_t value);
    bool _identify(uint8_t addr);
};

#endif // ARDUINO_ESP32P4_DEV
