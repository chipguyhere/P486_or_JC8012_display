/*******************************************************************************
 * LVGL Calculator — for the JC8012P4A1C or the P486 Panel
 *
 * One firmware image for both boards; the library works out which one it
 * booted on.  A dark, iOS-styled four-function calculator whose keypad is laid
 * out from the live display resolution (see ui.cpp): tall and roomy on the
 * JC8012's 800x1280 portrait panel, compact on the P486's 720x720 square.
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

// The calculator UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial0.begin(115200);

    // Native resolution, portrait, on both boards.
    lv_setup.begin({0, P4Res::Native},     // JC8012: 800x1280
                   {0, P4Res::Native});    // P486:   720x720
    Serial0.printf("LVGL initialized on the %s at %dx%d\n",
                   display.boardName(), display.width(), display.height());

    ui_init();
}

void loop() {
    lv_timer_handler();
    delay(5);
}
