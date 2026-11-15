/*
 * One display + touch driver for two ESP32-P4 panel boards, chosen at runtime:
 *
 *   JC8012  Guition JC8012P4A1C — 800x1280 JD9365 panel, GSL3680 touch
 *   P486    Waveshare ESP32-P4-86-Panel-ETH-2RO — 720x720 ST7703 panel, GT911
 *           touch, IP101 Ethernet
 *
 * The same firmware image runs on both.  At begin() the board is identified
 * (see chipguy_p4panel_detect() below), then brought up with whichever of the
 * two preferences the sketch gave for it — a rotation and a resolution each —
 * and width()/height() report the canvas that actually resulted, which is what
 * LVGL is told to render at.
 *
 * RESOLUTIONS
 *
 *   P4Res::Native     The panel's own pixels.  JC8012 800x1280 (1280x800 in
 *                     landscape); P486 720x720.
 *   P4Res::Square480  A 480x480 canvas the PPA scales up: x1.625 to a 780x780
 *                     square centred on the JC8012, x1.5 to fill the P486.
 *                     The same 480x480 UI then runs unchanged on both boards.
 *   P4Res::Full480    480 wide, using as much of the panel as fits: JC8012
 *                     480x768 (768x480 landscape) at x1.625; on the square
 *                     P486 this is the same as Square480.
 *
 * Rotation is 0/1/2/3, or the degrees 0/90/180/270 as aliases, and is done by
 * the ESP32-P4's PPA in hardware.  It follows the usual Arduino display
 * convention: 90 turns the picture clockwise, for a board mounted turned 90
 * degrees counter-clockwise; a board mounted turned clockwise needs 270.  The one case that bypasses the PPA is the
 * P486 at Native resolution and rotation 0: LVGL then renders straight into
 * the DMA framebuffers, with no copy and no extra PSRAM.
 *
 * Everything here is adapted from chipguy_JC8012P4A1C_display / _480display
 * and chipguy_P486Panel_display / _480display / _touch, including each board's
 * warm-boot recovery.  Those libraries are not needed, and should not be used
 * in the same sketch: they carry their own copies of the same C drivers.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

#ifndef BOARD_HAS_PSRAM
#error "chipguy_P486_or_JC8012_display requires PSRAM. Enable it via Tools > PSRAM: \"Enabled\"."
#endif

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/ppa.h"
#include "chipguy_ThreadsafeWire.h"
#include "p4panel_pins.h"
#include "gsl3680_touch.h"
#include "gt911_touch.h"

// ── Which board ─────────────────────────────────────────────────────────────

enum class P4Board : uint8_t { Unknown, JC8012, P486 };

// Identify the board.  Runs once per boot and caches the answer, so it is
// cheap to call from anywhere, before or after display.begin().
//
//   0. If an earlier boot stored the answer in NVS, use it.  Nothing is probed.
//   1. Otherwise probe the I2C bus (address only, nothing written).  GPIO7/8
//      are I2C on both boards; no other pin is touched.
//        0x32 answers (the JC8012's battery-backed RX8025T RTC)  -> JC8012
//        0x5D or 0x14 answers (the P486's GT911 touch)           -> P486
//      A definite answer here is stored in NVS for every later boot.
//   2. If neither or both answer, fall back to the flash chip's size, read
//      from its JEDEC ID: 32 MB -> P486, 16 MB -> JC8012.  Used for this boot
//      only; the next boot probes I2C again.
// Unknown means none of that worked; begin() then refuses to touch the panel.
P4Board chipguy_p4panel_detect();

// How the current answer was reached.
enum class P4Source : uint8_t { None, Nvs, I2c, Flash, Forced };
P4Source chipguy_p4panel_detect_source();
const char *chipguy_p4panel_source_name(P4Source source);

// Use this answer instead of detecting, for this boot only (nothing stored).
// Call before anything that would detect — display.begin(), or a NetClient
// configureEthernet() — e.g. to test one board's code path.
void chipguy_p4panel_force(P4Board board);

// Delete the stored answer, so the next detection (next boot) probes again.
void chipguy_p4panel_forget();

const char *chipguy_p4panel_name(P4Board board);   // "JC8012", "P486", "unknown"

// The shared I2C bus on GPIO7/8, mutex-protected.  Started by detection, at
// 400 kHz.  Use it for any other I2C device on either board (codec, RTC,
// headers): every transaction locks itself.  Never ::Wire.begin() on other
// pins — ::Wire IS this bus.
ThreadsafeWire &p4panel_wire();

// ── What the sketch would like on each board ────────────────────────────────

enum class P4Res : uint8_t { Native, Square480, Full480 };

struct P4Choice {
    uint16_t rotation = 0;          // 0/1/2/3, or 0/90/180/270; 90 = picture clockwise
    P4Res    res = P4Res::Native;
};

class chipguy_P486_or_JC8012_display {
public:
    chipguy_P486_or_JC8012_display();

    // Identify the board and bring up its panel, touch, framebuffers and PPA
    // with the matching preference.  Returns false if bring-up failed in a way
    // that left the sketch running (see lv_setup_P486_or_JC8012.hpp for what to
    // do then).  May not return at all on a P486 after a warm restart — see
    // setWarmBootQuiesce().
    bool begin(P4Choice jc8012, P4Choice p486);
    bool begin(P4Choice both) { return begin(both, both); }

    // ── What came up ────────────────────────────────────────────────────────
    P4Board board() const { return _board; }
    const char *boardName() const { return chipguy_p4panel_name(_board); }
    P4Res   resolution() const { return _res; }
    bool    isScaled() const { return _scale != 1.0f; }
    uint8_t rotation() const { return _rotation; }

    // The canvas LVGL renders at.
    int16_t width()  const { return _draw_w; }
    int16_t height() const { return _draw_h; }
    size_t  framebufferSize() const { return (size_t)_draw_w * _draw_h * sizeof(uint16_t); }

    // The physical panel, unrotated.
    int16_t nativeWidth()  const { return _native_w; }
    int16_t nativeHeight() const { return _native_h; }

    // Only the P486 has Ethernet.  chipguy_MQTT_netclient_P486Panel.h asks the
    // same question through a weak hook, so a sketch can include it and get
    // Wi-Fi on the JC8012 without asking.
    bool hasEthernet() const { return _board == P4Board::P486; }

    // ── Rendering ───────────────────────────────────────────────────────────
    // The two buffers LVGL renders into (DIRECT mode).  On the P486 at Native
    // resolution and rotation 0 these are the DMA framebuffers themselves.
    uint16_t *getDrawBuffer(uint8_t index);

    // A frame LVGL finished rendering into `pixelmap` (one of the two draw
    // buffers): put it on the panel.  Direct mode: cache sync and page flip.
    // Otherwise: hand it to the PPA task to scale/rotate into the next DMA
    // framebuffer, which then becomes the one shown.
    void present(const void *pixelmap);

    uint16_t *getFramebuffer(uint8_t index);
    void setActiveFramebuffer(uint8_t index, bool wait_for_vsync = true);
    bool waitVsync(uint32_t timeout_ms = 50);
    uint8_t getNumFramebuffers();
    bool isDirect() const { return _direct; }

    // ── Touch ───────────────────────────────────────────────────────────────
    // The current touch, mapped back through the PPA's offset, rotation and
    // scale into canvas coordinates — so it lines up with what LVGL drew.
    // False when not touched, or when the touch is outside the canvas (the
    // black border around a scaled JC8012 image).
    bool getTouch(int32_t &x, int32_t &y);
    bool touchAvailable() const { return _touch_ok; }

    // Start the JC8012's touch controller again, e.g. after its firmware was
    // installed in NVS while this sketch was running.  Updates touchAvailable().
    // The P486's GT911 needs no firmware: this just reports touchAvailable().
    bool restartTouch() {
        if (_board == P4Board::JC8012) _touch_ok = _gsl.begin();
        return _touch_ok;
    }

    // Touch steadiness filter, OFF by default.  On the JC8012 it turns on the
    // GSL3680 driver's phantom-jump rejection and 7-sample median, which keep
    // a spurious contact from yanking a slider to its extreme at the cost of
    // ~50 ms lag and of very fast swipes being cut short — right for a
    // home-control panel whose gestures are short, wrong as a default.  (See
    // gsl3680_touch.h.)  No effect on the P486: its GT911 has never needed
    // one.  Call before or after begin().
    void setTouchFilter(bool on) { _gsl.setFilter(on); }
    bool touchFilter() const { return _gsl.filter(); }

    // Map a raw NATIVE touch point to canvas coordinates.
    bool mapTouch(uint16_t rawX, uint16_t rawY, int32_t &lx, int32_t &ly) const;

    // ── Backlight ───────────────────────────────────────────────────────────
    // 0..100.  The P486's backlight gives no light below ~40% duty, so its
    // 1..100 are spread across the usable range.
    void setBacklight(int percentage);
    void setBacklight(bool on) { setBacklight(on ? 100 : 0); }

    // ── Screen off / sleep ──────────────────────────────────────────────────
    // JC8012: real low-power sleep.  The panel goes to sleep, the chip
    // light-sleeps, and the call returns ~170 ms after the wake with RAM and
    // the picture intact.  Measured battery current asleep (radio held off
    // with setRadioPower(false); add ~22 mA if it is running):
    //   sleepUntilSwipe()   ~16 mA at 3 s checks.  A finger RESTING on the
    //                       screen isn't seen — swipe for up to checkMs.
    //   sleepUntilTap()     ~45 mA.  Any touch wakes at once.
    //   sleepUntilButton()  ~10 mA.  Only BOOT wakes.
    // BOOT also wakes the first two.
    //
    // P486: screen off only.  The backlight goes dark and every variant waits
    // for a touch, polling it, with the chip awake (the board is mains / PoE
    // powered, and light sleep is not implemented for its panel).  Returns
    // WAKE_TAP.  BOOT is not used: it shares GPIO35 with Ethernet.
    enum WakeReason : uint8_t { WAKE_SWIPE, WAKE_TAP, WAKE_BUTTON };
    WakeReason sleepUntilSwipe(uint32_t checkMs = 3000);
    WakeReason sleepUntilTap();
    WakeReason sleepUntilButton();
    bool supportsLowPowerSleep() const { return _board == P4Board::JC8012; }

    // Panel off/on without sleeping the chip.  JC8012: Display Off + Sleep In
    // and the video stream paused.  P486: backlight only.
    void sleep();
    void wake();
    bool isAsleep() const { return _asleep; }

    // ── ESP32-C6 radio (GPIO54 on both boards) ──────────────────────────────
    // The C6 boots its Wi-Fi firmware at power-on and draws ~22 mA (measured on
    // the JC8012) whether or not the sketch uses Wi-Fi.  false holds it in
    // reset; true releases it.  The core's Wi-Fi drives the same pin, so only
    // use this in sketches that don't use Wi-Fi.
    void setRadioPower(bool on);

    // ── P486 warm-boot recovery ─────────────────────────────────────────────
    // The P486's panel can come up black after a WARM restart (ESP.restart(),
    // OTA, panic, software watchdog).  So on a P486, a boot that followed a
    // soft reset brings the display up and then promotes itself to a Super
    // Watchdog reset (core + RTC domain); the boot after that runs normally.
    // Measured 20/20 recoveries.  Costs ~1.5-2 s per soft reset, nothing on a
    // cold boot, and is bounded by both the reset reason and an NVS flag.
    // Anything setup() did before begin() runs twice when it happens.
    // Call before begin() to disable.  No effect on the JC8012.
    void setWarmBootQuiesce(bool enable) { _quiesce_on_warm_boot = enable; }

    // P486 recovery for a failed begin(): hold the panel in reset for hold_ms,
    // then restart.  Does not return.
    [[noreturn]] void resetAndRestart(uint32_t hold_ms = 10000);

    // The touch drivers, for anything this class doesn't wrap.
    gsl3680_touch &getGsl3680() { return _gsl; }
    gt911_touch   &getGt911()   { return _gt911; }

    // Debug counters from the DPI panel driver.
    uint32_t getFrameCount();
    uint32_t getUnderrunCount();

private:
    P4Board  _board = P4Board::Unknown;
    P4Res    _res = P4Res::Native;
    uint8_t  _rotation = 0;
    uint16_t _native_w = 0, _native_h = 0;
    uint16_t _draw_w = 0, _draw_h = 0;
    // Where the (scaled, rotated) canvas lands on the native panel.
    uint16_t _region_x = 0, _region_y = 0, _region_w = 0, _region_h = 0;
    float    _scale = 1.0f;
    bool     _direct = false;

    gsl3680_touch _gsl;
    gt911_touch   _gt911;
    bool _touch_ok = false;

    esp_ldo_channel_handle_t _ldo_mipi_phy = nullptr;
    esp_lcd_dsi_bus_handle_t _mipi_dsi_bus = nullptr;
    esp_lcd_panel_handle_t _panel_handle = nullptr;
    esp_lcd_panel_io_handle_t _io_handle = nullptr;
    ppa_client_handle_t _ppa_client = nullptr;
    uint16_t *_draw_buffers[2] = {nullptr, nullptr};
    uint8_t  _target_fb_index = 0;

    int  _backlight_pct = 100;     // last level set, restored by wake()
    bool _asleep = false;
    bool _quiesce_on_warm_boot = true;

    void _computeGeometry(P4Choice choice);
    bool _rawTouch(uint16_t &x, uint16_t &y);
    void _waitForLift();
    bool _allocateDrawBuffers();
    bool _clearFramebuffers(size_t fb_size);

    // JC8012 bring-up (see the .cpp for the reset-timing rationale).
    bool _beginJC8012();
    void _jcAssertPanelReset();
    void _jcReleasePanelFromReset();
    bool _jcPanelInitFailed(const char *step, esp_err_t err);
    WakeReason _jcLightSleep(uint8_t mode, uint32_t checkMs);

    // P486 bring-up.
    bool _beginP486();
    void _p486RecoveryResetPulse();
    bool _p486ShouldPromoteWarmBootReset();
    void _p486TriggerDeepestReset();
    WakeReason _p486ScreenOffUntilTouch();

    // Async PPA task: scales/rotates a draw buffer into a framebuffer and flips.
    void _flip(uint8_t draw_buf_index, uint8_t target_fb_index);
    static void _ppaTaskFunc(void *arg);
    TaskHandle_t _ppa_task_handle = nullptr;
    SemaphoreHandle_t _ppa_done_sem = nullptr;
    struct {
        uint8_t draw_buf_index;
        uint8_t target_fb_index;
    } _ppa_work;
};

#endif // ARDUINO_ESP32P4_DEV
