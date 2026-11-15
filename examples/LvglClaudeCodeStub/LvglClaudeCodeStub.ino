/*******************************************************************************
 * LVGL Claude Code Stub — for the JC8012P4A1C or the P486 Panel
 *
 * One firmware image for two ESP32-P4 boards:
 *   - Guition JC8012P4A1C   800x1280 JD9365 MIPI-DSI panel, GSL3680 touch
 *   - Waveshare P486 Panel  720x720 ST7703 MIPI-DSI panel, GT911 touch
 * The library works out which one it booted on and drives it.
 *
 * A minimal starting point for building an LVGL application from scratch.
 * The UI lives in a tiny ui.h / ui.cpp pair in the sketch folder that draws a
 * simple "Hello, world!" screen and lays itself out from the live resolution.
 *
 * The intent is to hand this to Claude Code (or any developer) as a clean
 * scaffold: the display, touch, and LVGL plumbing is already wired up, so you
 * can focus on writing your own UI in ui.cpp.
 *
 * Choosing a resolution: lv_setup.begin() takes one {rotation, resolution} per
 * board, and uses whichever matches.  Rotation is 0/1/2/3 or 0/90/180/270.
 * Resolution is one of:
 *   P4Res::Native     JC8012 800x1280 (1280x800 rotated); P486 720x720
 *   P4Res::Square480  480x480 on both, scaled up by the hardware
 *   P4Res::Full480    JC8012 480x768 (768x480 rotated); P486 480x480
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
 *   - USB CDC On Boot: Disabled
 *
 * Serial output: Serial0 is UART0, which on both boards comes out of the
 * USB-C port wired to the USB-serial chip — whatever "USB CDC On Boot" says.
 * That setting only decides what `Serial` means: Disabled makes it the same
 * UART as Serial0; Enabled makes it the ESP32-P4's own native USB port.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The UI for this example lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    Serial0.begin(115200);

    // Native resolution, portrait, on both boards.  For example,
    //   lv_setup.begin({90, P4Res::Native}, {0, P4Res::Square480});
    // would give a 1280x800 landscape JC8012 and a 480x480 P486.
    lv_setup.begin({0, P4Res::Native},     // JC8012
                   {0, P4Res::Native});    // P486
    Serial0.printf("LVGL initialized on the %s at %dx%d\n",
                   display.boardName(), display.width(), display.height());

    // Start the application's own setup
    ui_init();
}

void loop() {
    // Give loop control to LVGL objects created by the application
    lv_timer_handler();
    delay(5);
}
