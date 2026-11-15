/*******************************************************************************
 * LVGL Bouncing Balls — for the JC8012P4A1C or the P486 Panel
 *
 * One firmware image for both boards; the library works out which one it
 * booted on.  A resolution-independent demo: 1..20 circular balls bounce under
 * gravity, each a random color, re-launching to a random height on every floor
 * bounce.  Two sliders at the bottom select the number of balls and the speed.
 *
 * The whole UI is laid out from the live display resolution at runtime (see
 * ui.cpp), so it fills the JC8012's 800x1280 and the P486's 720x720 alike.
 * Try a different {rotation, resolution} per board in setup() — the demo
 * adapts to whatever comes up.
 *
 * Requires the following libraries:
 *   - chipguy_P486_or_JC8012_display  (this library)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings (the same for both boards):
 *   - Board: ESP32P4 Dev Module
 *   - PSRAM: Enabled   (REQUIRED — the sketch will not compile without it)
 *   - Flash Size: 16MB (128Mb)   (REQUIRED — the JC8012's size; fine on the
 *                                 P486's 32 MB part)
 *   - Partition Scheme: Custom (uses the partitions.csv in this sketch folder)
 *   - USB CDC On Boot: Disabled   (Serial0 — the USB-serial port — is used
 *                                 either way; see LvglClaudeCodeStub)
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The bouncing-balls UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial0.begin(115200);

    // Native resolution on both: 800x1280 portrait on a JC8012, 720x720 on a
    // P486.  e.g. {90, P4Res::Native} for the JC8012 would make it landscape.
    lv_setup.begin({0, P4Res::Native},     // JC8012
                   {0, P4Res::Native});    // P486
    Serial0.printf("LVGL initialized on the %s at %dx%d\n",
                   display.boardName(), display.width(), display.height());

    // Build the bouncing-balls screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
