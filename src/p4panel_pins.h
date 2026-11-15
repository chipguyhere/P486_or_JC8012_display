/*
 * Pin and address maps for the two boards chipguy_P486_or_JC8012_display drives.
 *
 * Both are ESP32-P4 boards with a 2-lane MIPI-DSI panel, an ESP32-C6 radio on
 * SDIO and an I2C bus on GPIO7/8 — but most other GPIOs mean different things
 * on each.  Several are actively dangerous to drive on the wrong board:
 *
 *   GPIO   JC8012P4A1C                    P486 Panel
 *   ----   ---------------------------    ---------------------------------
 *    23    LCD backlight (active HIGH)    GT911 touch reset
 *    26    WS2812 RGB LED                 LCD backlight (active LOW)
 *    52    battery-sense ADC              Ethernet MDIO
 *  28-31   FPC4 expansion header          Ethernet RMII (RXD0/1, CRS_DV, MDC)
 *  34/35   FPC4 / BOOT button             Ethernet TXD0/TXD1 (35 is also BOOT)
 *  49-51   (not used)                     Ethernet TX_EN, REF_CLK, PHY reset
 *
 * This is why nothing in the library touches a GPIO other than the I2C pins
 * until chipguy_p4panel_detect() has said which board it is running on.
 *
 * Sources: chipguy_JC8012P4A1C_board (JC8012P4A1C_Board.h, from Guition's
 * schematic) and chipguy_P486Panel_display / chipguy_MQTT_netclient_P486Panel.h
 * (from Waveshare's schematic).
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#include <stdint.h>

// Shared by both boards.
namespace P4PanelCommonPins {
    static constexpr int SDA = 7;
    static constexpr int SCL = 8;
    static constexpr int LCD_RST = 27;           // panel RESETn, both boards
    static constexpr int C6_RESET = 54;          // ESP32-C6 CHIP_PU, active LOW
    static constexpr int BOOT_BUTTON = 35;       // active LOW; on the P486 also Ethernet TXD1
    static constexpr int DSI_PHY_LDO_CHAN = 3;   // on-chip LDO, 2.5 V
}

namespace JC8012Pins {
    static constexpr int LCD_BACKLIGHT = 23;     // MP3202 boost enable, active HIGH
    static constexpr int TOUCH_RST = 22;
    static constexpr int TOUCH_INT = 21;
    static constexpr int RGB_LED = 26;           // one WS2812

    static constexpr uint8_t ES8311_ADDR  = 0x18;
    static constexpr uint8_t RX8025T_ADDR = 0x32;  // battery-backed RTC: the identifying device
    static constexpr uint8_t GSL3680_ADDR = 0x40;  // answers only once out of reset

    static constexpr uint16_t NATIVE_W = 800;
    static constexpr uint16_t NATIVE_H = 1280;
}

namespace P486Pins {
    static constexpr int LCD_BACKLIGHT = 26;     // active LOW
    static constexpr int TOUCH_RST = 23;         // GT911 reset, active LOW; INT not connected

    static constexpr uint8_t ES8311_ADDR = 0x18;
    static constexpr uint8_t ES7210_ADDR = 0x40;   // same address as the JC8012's GSL3680
    static constexpr uint8_t GT911_ADDR  = 0x5D;   // or 0x14: the identifying device

    static constexpr uint16_t NATIVE_W = 720;
    static constexpr uint16_t NATIVE_H = 720;
}
