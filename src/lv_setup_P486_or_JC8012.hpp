/*
 * lv_setup_P486_or_JC8012.hpp - Connects LVGL to whichever of the two ESP32-P4
 * panel boards the sketch boots on: the Guition JC8012P4A1C or the Waveshare
 * P486 Panel.
 *
 * This is the *actual* LVGL-to-driver glue.  It ships inside the library (src/)
 * so every example can share one copy.  Each example's sketch folder contains a
 * small lv_setup.hpp that simply #includes this file; that thin shim is the
 * file you edit if you want to take this code over and customize it.
 *
 * Include this file (or, normally, the example's lv_setup.hpp) after lvgl.h.
 *
 * WHICH BOARD, WHAT RESOLUTION
 *   lv_setup.begin() takes one preference per board, and uses the one that
 *   matches the board it finds itself on:
 *
 *       lv_setup.begin({90, P4Res::Native},      // on a JC8012: 1280x800
 *                      {0,  P4Res::Square480});  // on a P486:   480x480
 *
 *   Pass a single choice to use it on both.  Afterwards, display.width() and
 *   display.height() say what LVGL is actually rendering at, display.board()
 *   says which board it is, and display.hasEthernet() whether there is a jack.
 *   Lay the UI out from those (or from lv_display_get_horizontal_resolution())
 *   rather than from fixed numbers.
 *
 * LVGL runs in DIRECT render mode with two full-canvas buffers.  Each finished
 * frame goes to display.present(), which either page-flips (P486, Native,
 * rotation 0 — LVGL drew into the framebuffer itself) or has the PPA scale and
 * rotate it into the next framebuffer.  Touch needs no calibration on either
 * board: display.getTouch() maps the controller's native coordinates back
 * through the same scale and rotation.
 */

#pragma once

#ifdef CHIPGUY_LV_SETUP_INCLUDED
#error "Another lv_setup_*.hpp is already included - it also defines `display` and `lv_setup`."
#endif
#define CHIPGUY_LV_SETUP_INCLUDED

// One image has to run on both boards, so it is built for the smaller flash:
// the JC8012's 16 MB.  It runs fine on the P486's 32 MB part, leaving the top
// half unused.  A 32 MB build is the dangerous direction — the JC8012's
// bootloader refuses an image whose header claims more flash than the chip has.
//
// ESCAPE HATCH: define CHIPGUY_ALLOW_ANY_FLASH_SIZE (before including this) to
// waive the check, e.g. for a sketch that is only ever flashed to P486 boards.
#if defined(ARDUINO_FQBN) && !defined(CHIPGUY_ALLOW_ANY_FLASH_SIZE)
namespace {
    constexpr bool _cg_fqbn_contains(const char *hay, const char *needle) {
        for (const char *h = hay; *h; ++h) {
            const char *a = h, *b = needle;
            while (*a && *b && *a == *b) { ++a; ++b; }
            if (!*b) return true;
        }
        return false;
    }
}
static_assert(_cg_fqbn_contains(ARDUINO_FQBN, "FlashSize=16M"),
    "chipguy_P486_or_JC8012_display: set Tools > Flash Size to 16MB, so one image runs on both "
    "boards (the JC8012 has 16 MB). Define CHIPGUY_ALLOW_ANY_FLASH_SIZE to override.");
#endif

#include <chipguy_P486_or_JC8012_display.h>

static chipguy_P486_or_JC8012_display display;

class lv_setup_class {
public:
    // Optional callback invoked on every touch press (e.g. activity timeout).
    // It fires on each poll while a finger is held, so it repeats during a press.
    void (*onTouch)() = nullptr;

    // Initializes display, touch input, and LVGL.  Each P4Choice is
    // {rotation, resolution}; rotation is 0/1/2/3 or 0/90/180/270.
    void begin(P4Choice jc8012, P4Choice p486) {
        // WARNING: on a P486, display.begin() MAY NOT RETURN — after a warm
        // restart it brings the panel up and then deliberately reboots once,
        // more deeply, to clear the panel's black-screen fault (see
        // setWarmBootQuiesce() in the driver header).  So nothing setup() does
        // before this call may be a must-happen-exactly-once side effect.
        if (!display.begin(jc8012, p486)) {
            Serial0.printf("Display init failed (board: %s)\n", display.boardName());
            Serial0.flush();
            if (display.board() == P4Board::P486) {
                // Hold the panel in reset for 10 s and reboot, so an init
                // failure retries by itself.  Does not return.
                display.resetAndRestart(10000);
            }
            // JC8012: begin() already rebooted through its retry budget before
            // returning false.  Unknown board: nothing was touched.
            while (1) delay(100);
        }
        Serial0.printf("Display initialized: %s, %dx%d%s (board found by %s)\n",
                       display.boardName(), display.width(), display.height(),
                       display.isScaled() ? " scaled" : "",
                       chipguy_p4panel_source_name(chipguy_p4panel_detect_source()));

        _buf0 = display.getDrawBuffer(0);
        _buf1 = display.getDrawBuffer(1);

        lv_init();

        static lv_display_t *disp = lv_display_create(display.width(), display.height());
        lv_display_set_flush_cb(disp, _disp_flush);
        // Force RGB565 so the PPA's and the panel's color mode always match
        // what LVGL renders, however lv_conf.h's color depth resolved.
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, _buf0, _buf1, display.framebufferSize(),
                               LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_user_data(disp, this);

        static lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, _touchpad_read);
        lv_indev_set_user_data(indev, this);

        lv_tick_set_cb(_tick_get);
    }
    void begin(P4Choice both = P4Choice()) { begin(both, both); }

    // Access the underlying display driver (backlight, touch, etc.).
    chipguy_P486_or_JC8012_display &displayDriver() { return display; }

    // Screen off until woken (see the driver header for what each does on
    // each board).  The finger that wakes the board is swallowed — LVGL sees
    // no press until it lifts — so the waking touch can't hit a button.
    chipguy_P486_or_JC8012_display::WakeReason sleepUntilSwipe(uint32_t checkMs = 3000) {
        auto why = display.sleepUntilSwipe(checkMs); _afterWake(); return why;
    }
    chipguy_P486_or_JC8012_display::WakeReason sleepUntilTap() {
        auto why = display.sleepUntilTap(); _afterWake(); return why;
    }
    chipguy_P486_or_JC8012_display::WakeReason sleepUntilButton() {
        auto why = display.sleepUntilButton(); _afterWake(); return why;
    }

private:
    uint16_t *_buf0 = nullptr;
    uint16_t *_buf1 = nullptr;
    bool      _swallowUntilRelease = false;

    void _afterWake() {
        _swallowUntilRelease = true;
        lv_obj_invalidate(lv_screen_active());   // redraw anything changed while asleep
    }

    static uint32_t _tick_get(void) { return millis(); }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        // In DIRECT mode LVGL may call flush several times per refresh (one
        // per dirty area).  Only on the last one is the buffer complete.
        if (lv_display_flush_is_last(disp)) display.present(pixelmap);
        lv_display_flush_ready(disp);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        int32_t x, y;
        if (!display.getTouch(x, y)) {
            self->_swallowUntilRelease = false;   // the waking finger has lifted
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        if (self->_swallowUntilRelease) {         // still the finger that woke us
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (self->onTouch) self->onTouch();
    }
};

static lv_setup_class lv_setup;
