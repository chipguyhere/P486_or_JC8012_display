/*
 * One display + touch driver for the JC8012P4A1C and the P486 Panel.
 * See chipguy_P486_or_JC8012_display.h for the overview.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 */

#ifdef ARDUINO_ESP32P4_DEV

#include "chipguy_P486_or_JC8012_display.h"
#include "esp_lcd_panel_dpi_bb.h"
#include "esp_lcd_jd9365.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "hal/lpwdt_ll.h"      // LP_WDT_SWD_WKEY_VALUE
#include "soc/lp_wdt_struct.h"
#include <Preferences.h>

static const char *TAG = "P4Panel";

#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define MIPI_DPI_PX_FORMAT LCD_COLOR_PIXEL_FORMAT_RGB565
#define NUM_FRAMEBUFFERS 2

// The P4's data cache line.  esp_cache_msync() needs the start address AND
// size of every buffer it syncs to be a multiple of it.
#define CACHE_LINE 128

// ════════════════════════════════════════════════════════════════════════════
//  Board detection
// ════════════════════════════════════════════════════════════════════════════

static P4Board  s_board = P4Board::Unknown;
static P4Source s_source = P4Source::None;
static bool     s_detected = false;   // final for this boot, even if Unknown

// The answer is kept in NVS once I2C has given a definite one, and every later
// boot takes it from there without probing anything.  That removes the boot-
// time dependence on the I2C devices answering (a touch controller wedged by a
// warm reboot can hold SDA low) and on the flash size (a board revision could
// ship with a different part).  NVS survives ordinary serial and OTA uploads;
// an erase-all or chipguy_p4panel_forget() clears it.
#define P4PANEL_NVS_NS  "cgp4panel"
#define P4PANEL_NVS_KEY "board"

ThreadsafeWire &p4panel_wire()
{
    static ThreadsafeWire wire(::Wire, P4PanelCommonPins::SDA, P4PanelCommonPins::SCL,
                               "I2C0-GPIO7/8");
    return wire;
}

const char *chipguy_p4panel_name(P4Board board)
{
    switch (board) {
        case P4Board::JC8012: return "JC8012";
        case P4Board::P486:   return "P486";
        default:              return "unknown";
    }
}

const char *chipguy_p4panel_source_name(P4Source source)
{
    switch (source) {
        case P4Source::Nvs:    return "NVS";
        case P4Source::I2c:    return "I2C";
        case P4Source::Flash:  return "flash size";
        case P4Source::Forced: return "forced";
        default:               return "none";
    }
}

P4Source chipguy_p4panel_detect_source() { return s_source; }

void chipguy_p4panel_force(P4Board board)
{
    s_board = board;
    s_source = P4Source::Forced;
    s_detected = true;
    ESP_LOGW(TAG, "board forced to %s; detection skipped", chipguy_p4panel_name(board));
}

void chipguy_p4panel_forget()
{
    Preferences prefs;
    if (prefs.begin(P4PANEL_NVS_NS, false)) {
        prefs.remove(P4PANEL_NVS_KEY);
        prefs.end();
    }
    ESP_LOGW(TAG, "stored board identity cleared; the next detection probes again");
}

static P4Board probe_board(P4Source &source)
{
    // 1. Who answers on the I2C bus.  Address-only probes write nothing, and
    //    GPIO7/8 are I2C on both boards, so this is safe on either.
    ThreadsafeWire &wire = p4panel_wire();
    bool rtc = false, gt911 = false;
    if (wire.begin()) {
        wire.setClock(400000);
        rtc   = wire.probe(JC8012Pins::RX8025T_ADDR);
        gt911 = wire.probe(gt911_touch::ADDR_PRIMARY) || wire.probe(gt911_touch::ADDR_SECONDARY);
    } else {
        ESP_LOGE(TAG, "detect: I2C bus on GPIO7/8 failed to start");
    }

    // 2. The flash chip's real size (from its JEDEC ID, not the build setting).
    //    A GT911 or GSL3680 left clamping SDA by a warm reboot can make step 1
    //    come back empty; this needs nothing but the SPI flash.
    uint32_t flash = 0;
    esp_flash_get_physical_size(NULL, &flash);
    P4Board byFlash = flash >= 32u * 1024 * 1024 ? P4Board::P486
                    : flash >= 16u * 1024 * 1024 ? P4Board::JC8012
                    : P4Board::Unknown;

    P4Board byI2c = (rtc && !gt911) ? P4Board::JC8012
                  : (gt911 && !rtc) ? P4Board::P486
                  : P4Board::Unknown;

    ESP_LOGI(TAG, "detect: RTC@0x32 %s, GT911 %s, flash %u MB",
             rtc ? "yes" : "no", gt911 ? "yes" : "no", (unsigned)(flash >> 20));
    if (byI2c != P4Board::Unknown && byFlash != P4Board::Unknown && byI2c != byFlash) {
        ESP_LOGW(TAG, "detect: I2C says %s but flash size says %s; going with I2C",
                 chipguy_p4panel_name(byI2c), chipguy_p4panel_name(byFlash));
    }
    if (byI2c != P4Board::Unknown) {
        source = P4Source::I2c;
        return byI2c;
    }
    source = byFlash != P4Board::Unknown ? P4Source::Flash : P4Source::None;
    return byFlash;
}

P4Board chipguy_p4panel_detect()
{
    if (s_detected) return s_board;

    Preferences prefs;
    bool nvs = prefs.begin(P4PANEL_NVS_NS, false);
    if (nvs) {
        uint8_t stored = prefs.getUChar(P4PANEL_NVS_KEY, 0);
        if (stored == (uint8_t)P4Board::JC8012 || stored == (uint8_t)P4Board::P486) {
            prefs.end();
            s_board = (P4Board)stored;
            s_source = P4Source::Nvs;
            s_detected = true;
            ESP_LOGI(TAG, "detect: %s (stored in NVS)", chipguy_p4panel_name(s_board));
            return s_board;
        }
    }

    P4Source source;
    P4Board board = probe_board(source);

    // Only an I2C answer is worth keeping.  A flash-size answer is used for
    // this boot, but the next boot probes again rather than locking it in.
    if (nvs && source == P4Source::I2c) {
        prefs.putUChar(P4PANEL_NVS_KEY, (uint8_t)board);
        ESP_LOGI(TAG, "detect: stored %s in NVS", chipguy_p4panel_name(board));
    }
    if (nvs) prefs.end();

    s_board = board;
    s_source = source;
    s_detected = true;
    ESP_LOGI(TAG, "detect: %s (by %s)", chipguy_p4panel_name(board),
             chipguy_p4panel_source_name(source));
    return board;
}

// chipguy_MQTT_netclient_P486Panel.h declares this weak and asks it before
// configuring Ethernet.  Defined here, in the same object file as begin(),
// so it is always linked whenever the sketch uses the display.  An unknown
// board counts as "absent": driving the P486's Ethernet pins on anything else
// is the outcome to avoid.
extern "C" bool chipguy_netclient_ethernet_absent()
{
    return chipguy_p4panel_detect() != P4Board::P486;
}

// ════════════════════════════════════════════════════════════════════════════
//  Common
// ════════════════════════════════════════════════════════════════════════════

static uint8_t normalize_rotation(uint16_t rotation)
{
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

// Rotation follows the usual Arduino display convention: 1 (90) turns the
// picture 90 degrees CLOCKWISE on the panel - i.e. it is upright when the
// board is mounted turned 90 degrees counter-clockwise.  The PPA's angles are
// counter-clockwise, so 1 and 3 swap on the way in.
static ppa_srm_rotation_angle_t ppa_angle_for(uint8_t rotation)
{
    switch (rotation) {
        case 1:  return PPA_SRM_ROTATION_ANGLE_270;
        case 2:  return PPA_SRM_ROTATION_ANGLE_180;
        case 3:  return PPA_SRM_ROTATION_ANGLE_90;
        default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

chipguy_P486_or_JC8012_display::chipguy_P486_or_JC8012_display()
    : _gsl(p4panel_wire(), JC8012Pins::TOUCH_RST, JC8012Pins::TOUCH_INT),
      _gt911(p4panel_wire(), P486Pins::TOUCH_RST)
{
}

// Canvas size and where it lands on the panel, for a board + preference.
//
//   JC8012 (800x1280)  Native    canvas = panel, rotated; region = panel
//                      Square480 480x480 x1.625 -> 780x780 at (10, 250)
//                      Full480   480x768 / 768x480 x1.625 -> 780x1248 at (10, 16)
//   P486   (720x720)   Native    canvas = region = panel
//                      Square480 / Full480: 480x480 x1.5 -> 720x720 at (0, 0)
//
// Under 90/270 the PPA transposes its output, which is why a landscape canvas
// lands on the same (portrait) region as its portrait twin.  The scale ratios
// are exact in the PPA's fixed point: 1.625 = 26/16, 1.5 = 24/16.
void chipguy_P486_or_JC8012_display::_computeGeometry(P4Choice choice)
{
    _rotation = normalize_rotation(choice.rotation);
    _res = choice.res;
    bool landscape = (_rotation == 1 || _rotation == 3);

    if (_board == P4Board::JC8012) {
        _native_w = JC8012Pins::NATIVE_W;
        _native_h = JC8012Pins::NATIVE_H;
        if (_res == P4Res::Native) {
            _draw_w = landscape ? _native_h : _native_w;
            _draw_h = landscape ? _native_w : _native_h;
            _region_w = _native_w;  _region_h = _native_h;
            _scale = 1.0f;
        } else if (_res == P4Res::Square480) {
            _draw_w = _draw_h = 480;
            _region_w = _region_h = 780;
            _scale = 1.625f;
        } else {
            _draw_w = landscape ? 768 : 480;
            _draw_h = landscape ? 480 : 768;
            _region_w = 780;  _region_h = 1248;
            _scale = 1.625f;
        }
    } else {
        _native_w = P486Pins::NATIVE_W;
        _native_h = P486Pins::NATIVE_H;
        _region_w = _native_w;  _region_h = _native_h;
        if (_res == P4Res::Native) {
            _draw_w = _native_w;  _draw_h = _native_h;
            _scale = 1.0f;
        } else {
            _res = P4Res::Square480;      // the panel is square: Full480 is the same
            _draw_w = _draw_h = 480;
            _scale = 1.5f;
        }
    }
    _region_x = (_native_w - _region_w) / 2;
    _region_y = (_native_h - _region_h) / 2;

    // Native, unrotated, no scale: render straight into the DMA framebuffers.
    // Only the P486 takes this path; the JC8012 driver has always gone through
    // the PPA (its async flip lets LVGL start the next frame sooner).
    _direct = (_board == P4Board::P486 && _scale == 1.0f && _rotation == 0);
}

bool chipguy_P486_or_JC8012_display::begin(P4Choice jc8012, P4Choice p486)
{
    _board = chipguy_p4panel_detect();
    if (_board == P4Board::Unknown) {
        ESP_LOGE(TAG, "begin(): could not tell which board this is; not touching any pins");
        return false;
    }
    // Detection normally started the bus already; not if the board was forced.
    // begin() is idempotent.
    if (p4panel_wire().begin()) p4panel_wire().setClock(400000);
    _computeGeometry(_board == P4Board::JC8012 ? jc8012 : p486);
    ESP_LOGI(TAG, "begin(): %s, rotation %d, canvas %dx%d, scale %.3f%s",
             boardName(), _rotation, _draw_w, _draw_h, (double)_scale,
             _direct ? ", direct" : "");

    return _board == P4Board::JC8012 ? _beginJC8012() : _beginP486();
}

bool chipguy_P486_or_JC8012_display::_clearFramebuffers(size_t fb_size)
{
    for (int i = 0; i < NUM_FRAMEBUFFERS; i++) {
        uint16_t *fb = getFramebuffer(i);
        if (!fb) return false;
        memset(fb, 0, fb_size);
        esp_cache_msync(fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    return true;
}

// Two canvas-sized draw buffers (PSRAM) plus the PPA client.  Not used in
// direct mode, where LVGL draws into the framebuffers.
bool chipguy_P486_or_JC8012_display::_allocateDrawBuffers()
{
    size_t buf_size = (framebufferSize() + CACHE_LINE - 1) & ~(size_t)(CACHE_LINE - 1);
    for (int i = 0; i < 2; i++) {
        _draw_buffers[i] = (uint16_t *)heap_caps_aligned_alloc(CACHE_LINE, buf_size,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_draw_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate %dx%d draw buffer %d", _draw_w, _draw_h, i);
            return false;
        }
        memset(_draw_buffers[i], 0, buf_size);
        esp_cache_msync(_draw_buffers[i], buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ESP_LOGI(TAG, "%dx%d draw buffer %d @ %p", _draw_w, _draw_h, i, _draw_buffers[i]);
    }

    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };
    esp_err_t ret = ppa_register_client(&ppa_cfg, &_ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA client registration failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  JC8012P4A1C bring-up (from chipguy_JC8012P4A1C_display)
// ════════════════════════════════════════════════════════════════════════════

// Panel reset timing.
//
// The JD9365 is held in reset for the whole host-side bring-up (DSI PHY LDO,
// DSI bus, DBI IO, DPI panel) rather than being pulsed afterwards.  That gives
// the assertion a much longer floor than the old 10 ms pulse, and keeps the
// panel from watching the D-PHY lanes come alive underneath it.
//
// This matters most on a warm reboot.  A watchdog reset restarts the SoC but
// never power-cycles the panel: it keeps its rails and whatever internal state
// it was in when the DSI stream stopped mid-transfer.  There is no panel
// power-enable GPIO on this board, so the reset line is the only recovery
// lever firmware has.
#define JC_RESET_MIN_LOW_MS  50    // floor on the low assertion during init
#define JC_RESET_SETTLE_MS   200   // after release, before the first command
#define JC_RESET_RECOVERY_MS 1000  // long assertion before a failure reboot

// How many consecutive failed bring-ups to reboot through before giving up.
// Without a bound, a permanently dead panel would reboot-loop forever, which
// hides the serial log and makes the board undiagnosable.
#define JC_INIT_MAX_ATTEMPTS 3

// Consecutive failed display bring-ups.  RTC_NOINIT memory survives a software
// reset (both our own esp_restart() and a watchdog reboot) but holds garbage
// after a true power-on, hence the magic-value seed.  Cleared on every
// successful begin(), so the budget is "consecutive failures", not "failures
// ever".
#define JC_ATTEMPT_MAGIC 0x4A433830u
RTC_NOINIT_ATTR static uint32_t s_jc_init_magic;
RTC_NOINIT_ATTR static uint32_t s_jc_init_attempts;

void chipguy_P486_or_JC8012_display::_jcAssertPanelReset()
{
    gpio_config_t rst_gpio_config = {
        .pin_bit_mask = 1ULL << P4PanelCommonPins::LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&rst_gpio_config);
    if (ret != ESP_OK) {
        // Not fatal on its own, but every recovery path depends on this pin.
        ESP_LOGE(TAG, "Reset GPIO config failed: %s", esp_err_to_name(ret));
    }
    gpio_set_level((gpio_num_t)P4PanelCommonPins::LCD_RST, 0);
}

void chipguy_P486_or_JC8012_display::_jcReleasePanelFromReset()
{
    gpio_set_level((gpio_num_t)P4PanelCommonPins::LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(JC_RESET_SETTLE_MS));
}

// Failure path for any step of the JC8012 bring-up.  Parks the panel in reset
// with the backlight off, holds it there far longer than a normal init pulse,
// and reboots so the whole sequence gets a clean run.  Bounded by
// JC_INIT_MAX_ATTEMPTS: once the budget is spent this returns instead of
// rebooting, leaving the board halted with a readable serial log.  Always
// returns false.  Pass ESP_OK for steps that have no esp_err_t of their own.
bool chipguy_P486_or_JC8012_display::_jcPanelInitFailed(const char *step, esp_err_t err)
{
    if (err == ESP_OK) {
        ESP_LOGE(TAG, "Display init failed at: %s", step);
    } else {
        ESP_LOGE(TAG, "Display init failed at: %s (%s)", step, esp_err_to_name(err));
    }

    setBacklight(0);
    _jcAssertPanelReset();

    if (s_jc_init_attempts >= JC_INIT_MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "Giving up after %u consecutive attempts. "
                      "Panel left in reset, backlight off.",
                 (unsigned)s_jc_init_attempts);
        return false;
    }

    ESP_LOGE(TAG, "Holding panel in reset for %d ms, then rebooting (attempt %u of %d)",
             JC_RESET_RECOVERY_MS, (unsigned)s_jc_init_attempts, JC_INIT_MAX_ATTEMPTS);
    vTaskDelay(pdMS_TO_TICKS(JC_RESET_RECOVERY_MS));
    esp_restart();
    return false;  // not reached
}

bool chipguy_P486_or_JC8012_display::_beginJC8012()
{
    // Seed or advance the consecutive-failure budget.  A cold boot leaves
    // RTC_NOINIT memory undefined, so key the seed off both the magic value
    // and the reset reason.
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (s_jc_init_magic != JC_ATTEMPT_MAGIC || reset_reason == ESP_RST_POWERON) {
        s_jc_init_magic = JC_ATTEMPT_MAGIC;
        s_jc_init_attempts = 0;
    }
    s_jc_init_attempts++;
    ESP_LOGI(TAG, "JC8012: reset reason %d, display init attempt %u",
             (int)reset_reason, (unsigned)s_jc_init_attempts);

    esp_err_t ret;
    const size_t fb_size = (size_t)_native_w * _native_h * sizeof(uint16_t);

    // Assert reset before anything else and hold it for the whole host-side
    // bring-up below.
    _jcAssertPanelReset();
    TickType_t reset_asserted_at = xTaskGetTickCount();

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = P4PanelCommonPins::DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &_ldo_mipi_phy);
    if (ret != ESP_OK) return _jcPanelInitFailed("esp_ldo_acquire_channel (MIPI DSI PHY)", ret);

    // Backlight off during init
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << JC8012Pins::LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ret = gpio_config(&bk_gpio_config);
    if (ret != ESP_OK) return _jcPanelInitFailed("gpio_config (backlight)", ret);
    gpio_set_level((gpio_num_t)JC8012Pins::LCD_BACKLIGHT, 0);

    esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ret = esp_lcd_new_dsi_bus(&bus_config, &_mipi_dsi_bus);
    if (ret != ESP_OK) return _jcPanelInitFailed("esp_lcd_new_dsi_bus", ret);

    esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ret = esp_lcd_new_panel_io_dbi(_mipi_dsi_bus, &dbi_config, &_io_handle);
    if (ret != ESP_OK) return _jcPanelInitFailed("esp_lcd_new_panel_io_dbi", ret);

    chipguy_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .pixel_format = MIPI_DPI_PX_FORMAT,
        .in_color_format = (lcd_color_format_t)0,
        .out_color_format = (lcd_color_format_t)0,
        .num_fbs = NUM_FRAMEBUFFERS,
        .video_timing = {
            .h_size = _native_w,
            .v_size = _native_h,
            .hsync_pulse_width = 20,
            .hsync_back_porch = 20,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 20,
        },
        .virtual_v_pixels = 0,
        .flags = {
            .use_dma2d = false,
            .disable_lp = false,
        },
    };
    ret = chipguy_lcd_new_panel_dpi(_mipi_dsi_bus, &dpi_config, &_panel_handle);
    if (ret != ESP_OK) return _jcPanelInitFailed("chipguy_lcd_new_panel_dpi", ret);

    // Host side is up; release the panel from reset, topping up the low time
    // only if the bring-up above came in under the minimum.
    TickType_t held_ticks = xTaskGetTickCount() - reset_asserted_at;
    TickType_t min_low_ticks = pdMS_TO_TICKS(JC_RESET_MIN_LOW_MS);
    if (held_ticks < min_low_ticks) vTaskDelay(min_low_ticks - held_ticks);
    _jcReleasePanelFromReset();

    uint8_t lcd_id[3] = {0};
    esp_lcd_panel_io_rx_param(_io_handle, 0x04, lcd_id, 3);
    ESP_LOGI(TAG, "LCD ID: %02X %02X %02X", lcd_id[0], lcd_id[1], lcd_id[2]);

    // MADCTL 0xC0 (MY|MX) sets the panel's base scan orientation; logical
    // rotation is layered on top by the PPA.
    esp_lcd_panel_io_tx_param(_io_handle, 0xE0, (uint8_t[]){0x00}, 1);
    esp_lcd_panel_io_tx_param(_io_handle, 0x36, (uint8_t[]){0xC0}, 1);
    esp_lcd_panel_io_tx_param(_io_handle, 0x3A, (uint8_t[]){0x55}, 1);
    esp_lcd_panel_io_tx_param(_io_handle, 0x80, (uint8_t[]){0x01}, 1);

    extern const jd9365_lcd_init_cmd_t vendor_specific_init_default[];
    extern const size_t vendor_specific_init_default_size;
    for (size_t i = 0; i < vendor_specific_init_default_size; i++) {
        const jd9365_lcd_init_cmd_t *cmd = &vendor_specific_init_default[i];
        esp_lcd_panel_io_tx_param(_io_handle, cmd->cmd, cmd->data_bytes ? cmd->data : NULL,
                                  cmd->data_bytes);
        if (cmd->delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }

    ret = esp_lcd_panel_init(_panel_handle);
    if (ret != ESP_OK) return _jcPanelInitFailed("esp_lcd_panel_init", ret);

    // Sleep Out and Display On after video mode is enabled
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_lcd_panel_io_tx_param(_io_handle, 0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(_io_handle, 0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    _clearFramebuffers(fb_size);

    // Touch.  A watchdog reboot can catch the GSL3680's I2C bus mid-transaction
    // and leave it wedged, so a failure here is worth a reset-and-retry while
    // the budget lasts.  Once it is spent, come up with a working display and
    // touch disabled rather than a dark panel.
    if (_gsl.begin()) {
        _touch_ok = true;
    } else if (_gsl.retryMightHelp() && s_jc_init_attempts < JC_INIT_MAX_ATTEMPTS) {
        // Only a controller that didn't answer is worth a reboot.  Firmware
        // missing from NVS won't appear on its own: come up without touch.
        return _jcPanelInitFailed("gsl3680 touch init", ESP_OK);
    } else {
        _touch_ok = false;
        ESP_LOGE(TAG, "Touch unavailable (%s); continuing with touch disabled.",
                 gsl3680_touch::statusText(_gsl.status()));
    }

    if (!_allocateDrawBuffers()) {
        return _jcPanelInitFailed("draw buffer / PPA client allocation", ESP_OK);
    }

    pinMode(P4PanelCommonPins::BOOT_BUTTON, INPUT);
    setBacklight(100);
    s_jc_init_attempts = 0;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  P486 Panel bring-up (from chipguy_P486Panel_display)
// ════════════════════════════════════════════════════════════════════════════

#define P486_HSYNC_PULSE_WIDTH 20
#define P486_HSYNC_BACK_PORCH 80
#define P486_HSYNC_FRONT_PORCH 80
#define P486_VSYNC_PULSE_WIDTH 4
#define P486_VSYNC_BACK_PORCH 12
#define P486_VSYNC_FRONT_PORCH 30
#define P486_DPI_CLOCK_MHZ 46
#define P486_LANE_BIT_RATE_MBPS 600

// Recovery reset pulse shape.  See _p486RecoveryResetPulse().
#define P486_RST_LOW_MS     100   // RESETn held low
#define P486_RST_HIGH_MS    100   // RESETn driven high before being released
#define P486_RST_QUIET_MS   100   // pin left undriven, nothing else touched, after the pulse

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} p486_init_cmd_t;

// ST7703 vendor-specific initialization commands
static const p486_init_cmd_t p486_init_cmds[] = {
    {0xB9, (uint8_t[]){0xF1, 0x12, 0x83}, 3, 0},
    {0xBA, (uint8_t[]){0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37}, 27, 0},
    {0xB8, (uint8_t[]){0x25, 0x22, 0xF0, 0x63}, 4, 0},
    {0xBF, (uint8_t[]){0x02, 0x11, 0x00}, 3, 0},
    {0xB3, (uint8_t[]){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xC0, (uint8_t[]){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00}, 9, 0},
    {0xBC, (uint8_t[]){0x46}, 1, 0},
    {0xCC, (uint8_t[]){0x0B}, 1, 0},
    {0xB4, (uint8_t[]){0x80}, 1, 0},
    {0xB2, (uint8_t[]){0x3C, 0x12, 0x30}, 3, 0},
    {0xE3, (uint8_t[]){0x07, 0x07, 0x0B, 0x0B, 0x03, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10}, 14, 0},
    {0xC1, (uint8_t[]){0x36, 0x00, 0x32, 0x32, 0x77, 0xF1, 0xCC, 0xCC, 0x77, 0x77, 0x33, 0x33}, 12, 0},
    {0xB5, (uint8_t[]){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t[]){0xB2, 0xB2}, 2, 0},
    {0xE9, (uint8_t[]){0xC8, 0x10, 0x0A, 0x10, 0x0F, 0xA1, 0x80, 0x12, 0x31, 0x23, 0x47, 0x86, 0xA1, 0x80, 0x47, 0x08, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x48, 0x02, 0x8B, 0xAF, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x48, 0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 63, 0},
    {0xEA, (uint8_t[]){0x96, 0x12, 0x01, 0x01, 0x01, 0x78, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0x31, 0x8B, 0xA8, 0x31, 0x75, 0x88, 0x88, 0x88, 0x88, 0x88, 0x4F, 0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88, 0x88, 0x88, 0x88, 0x88, 0x23, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xA1, 0x80, 0x00, 0x00, 0x00, 0x00}, 61, 0},
    {0xE0, (uint8_t[]){0x00, 0x0A, 0x0F, 0x29, 0x3B, 0x3F, 0x42, 0x39, 0x06, 0x0D, 0x10, 0x13, 0x15, 0x14, 0x15, 0x10, 0x17, 0x00, 0x0A, 0x0F, 0x29, 0x3B, 0x3F, 0x42, 0x39, 0x06, 0x0D, 0x10, 0x13, 0x15, 0x14, 0x15, 0x10, 0x17}, 34, 0},
    {0x11, (uint8_t[]){0x00}, 1, 250},
    {0x29, (uint8_t[]){0x00}, 1, 50},
};

// ---------------------------------------------------------------------------
// Warm-boot reset promotion
//
// The panel intermittently comes up black -- backlight on, firmware running,
// LVGL rendering, the DPI engine scanning real pixels out at full rate, and the
// panel reporting sleep-out and display-on.  Every host-side measurement reads
// identical to a working boot.  Only the picture is missing.
//
// Measured on natural failures (not induced):
//   - light sleep with TOP+CNNT powered down, plus a full DSI rebuild:  no
//   - deep sleep, which powers the entire digital core and PHY down:    no
//   - RWDT reset (rst:0x10 SYS_RWDT):     recovered ONCE, then measured
//                                         firing twice on a black field unit
//                                         with the screen staying black
//   - EN button (whole-chip reset):       reliably recovers
//
// The Super Watchdog (rst:0x12, documented as resetting "the digital core and
// rtc module") is the deepest cause reachable from software.  It fires in
// ~1.8 s and boots cleanly.
//
// So: on a soft reset, bring the display up and then, with the link still
// running, promote to a Super Watchdog reset.  Measured 2026-08-18: 20/20
// black panels recovered across 20 soft resets this way.  The same reset after
// an orderly DSI teardown recovered 0/11, so there is deliberately no
// teardown.
//
// LOOP SAFETY.  Two independent guards:
//   1. Reset reason.  Our promotion comes back as ESP_RST_WDT, which is
//      deliberately absent from the promote list below.
//   2. An NVS flag, set before promoting and cleared on the next boot, which
//      bounds promotion to at most one per soft reset no matter what the
//      reason reads as.  (RTC RAM can't hold it: these resets wipe it.)
// The NVS namespace is the same one chipguy_P486Panel_display uses, so the
// guard holds across a reflash between the two libraries.
// ---------------------------------------------------------------------------
bool chipguy_P486_or_JC8012_display::_p486ShouldPromoteWarmBootReset()
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool promote = (reason == ESP_RST_SW       || reason == ESP_RST_PANIC ||
                    reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT);

    Preferences prefs;
    if (prefs.begin("cgp486", false)) {
        if (prefs.getBool("promoted", false)) {
            prefs.putBool("promoted", false);
            prefs.end();
            ESP_LOGE(TAG, "warm boot (reason %d): promotion already used this cycle, "
                          "continuing with normal bring-up", (int)reason);
            return false;
        }
        prefs.end();
    } else if (promote) {
        ESP_LOGE(TAG, "NVS unavailable; relying on the reset-reason guard alone");
    }

    if (promote) {
        ESP_LOGE(TAG, "warm boot (reset reason %d): will bring DSI up, then promote "
                      "to a Super Watchdog reset", (int)reason);
    }
    return promote;
}

// Fire the Super Watchdog (digital core + RTC domain).  Does not return in the
// normal case; if the dog never bites we fall through and the caller carries on.
void chipguy_P486_or_JC8012_display::_p486TriggerDeepestReset()
{
    Preferences prefs;
    if (prefs.begin("cgp486", false)) {
        prefs.putBool("promoted", true);
        prefs.end();
    }

    // The SWD has no timeout register -- it is a fixed hardware timer.  Arm it
    // by clearing swd_disable and swd_auto_feed_en, then simply stop feeding.
    LP_WDT.swd_wprotect.swd_wkey = LP_WDT_SWD_WKEY_VALUE;
    LP_WDT.swd_config.swd_auto_feed_en = 0;
    LP_WDT.swd_config.swd_disable = 0;
    LP_WDT.swd_wprotect.swd_wkey = 0;

    uint32_t start = millis();
    while (millis() - start < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "super watchdog did not fire within 5 s; continuing");
}

// Panel recovery reset pulse, before the DSI PHY, the bus or anything else is
// touched.  Measured in the field 2026-09-03: units that come up black stay
// black through a soft reset AND through an EN-pin reset, but are recovered
// reliably by driving RESETn low 100 ms, high 100 ms, and then RELEASING the
// pin -- leaving it undriven, as a plain input with no pull, which is exactly
// what was tested -- before any initialization is attempted.
void chipguy_P486_or_JC8012_display::_p486RecoveryResetPulse()
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P4PanelCommonPins::LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)P4PanelCommonPins::LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(P486_RST_LOW_MS));
    gpio_set_level((gpio_num_t)P4PanelCommonPins::LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(P486_RST_HIGH_MS));

    io_conf.mode = GPIO_MODE_INPUT;      // Hi-Z: the step the recovery depends on
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(P486_RST_QUIET_MS));
}

bool chipguy_P486_or_JC8012_display::_beginP486()
{
    const int lcd_rst_pin = P4PanelCommonPins::LCD_RST;
    const size_t fb_size = (size_t)_native_w * _native_h * sizeof(uint16_t);

    _p486RecoveryResetPulse();

    // Assert panel reset across the host-side re-init, so a panel that has not
    // been power-cycled stays quiescent while the DSI link comes back up.
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << lcd_rst_pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)lcd_rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Decide now whether this boot ends in a promoted reset; the reset itself
    // is deferred to the bottom.  The recovery pulse above clears some black
    // panels but, measured, not all of them -- this is the fallback.
    bool promote_pending = _quiesce_on_warm_boot && _p486ShouldPromoteWarmBootReset();

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = P4PanelCommonPins::DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &_ldo_mipi_phy));

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_PLL_F20M,
        .lane_bit_rate_mbps = (float)P486_LANE_BIT_RATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &_mipi_dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(_mipi_dsi_bus, &dbi_config, &_io_handle));

    chipguy_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = P486_DPI_CLOCK_MHZ,
        .pixel_format = MIPI_DPI_PX_FORMAT,
        .in_color_format = (lcd_color_format_t)0,
        .out_color_format = (lcd_color_format_t)0,
        .num_fbs = NUM_FRAMEBUFFERS,
        .video_timing = {
            .h_size = _native_w,
            .v_size = _native_h,
            .hsync_pulse_width = P486_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = P486_HSYNC_BACK_PORCH,
            .hsync_front_porch = P486_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = P486_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = P486_VSYNC_BACK_PORCH,
            .vsync_front_porch = P486_VSYNC_FRONT_PORCH,
        },
        .virtual_v_pixels = 0,
        .flags = {
            .use_dma2d = false,
            .disable_lp = false,
        },
    };
    ESP_ERROR_CHECK(chipguy_lcd_new_panel_dpi(_mipi_dsi_bus, &dpi_config, &_panel_handle));

    // Release reset now that the DSI link is up; the ST7703 wants >=120 ms
    // before its first command.
    gpio_set_level((gpio_num_t)lcd_rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(p486_init_cmds) / sizeof(p486_init_cmds[0]); i++) {
        const p486_init_cmd_t *cmd = &p486_init_cmds[i];
        esp_lcd_panel_io_tx_param(_io_handle, cmd->cmd, cmd->data_bytes ? cmd->data : NULL,
                                  cmd->data_bytes);
        if (cmd->delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
    }

    esp_err_t ret = esp_lcd_panel_init(_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
        return false;
    }
    _clearFramebuffers(fb_size);

    // Rotation or scaling cannot be done in place: give LVGL its own buffers
    // and let the PPA write the framebuffers.  Direct mode needs neither.
    if (!_direct && !_allocateDrawBuffers()) return false;

    // Promoted reset, deferred to here on purpose: at the top of begin() the
    // DSI hardware is still running from the previous image with no software
    // owning it, and a reset there was measured NOT to prevent a black
    // bring-up.  Here the link is up and owned.  It is NOT torn down first.
    if (promote_pending) {
        _p486TriggerDeepestReset();     // normally does not return
        ESP_LOGE(TAG, "promoted reset did not happen; reporting init failure");
        return false;
    }

    // Touch.  The original driver kept running without it, and so does this.
    _touch_ok = _gt911.begin();

    // chipguy_P486Panel_display never set the backlight in begin(): it is on
    // by default.  Leave it alone here too; setBacklight() takes over when
    // the sketch calls it.
    _backlight_pct = 100;
    return true;
}

void chipguy_P486_or_JC8012_display::resetAndRestart(uint32_t hold_ms)
{
    ESP_LOGE(TAG, "Display init failed; holding panel in reset for %u ms, then restarting",
             (unsigned)hold_ms);
    if (_board != P4Board::Unknown) setBacklight(0);

    // A chip restart does not power-cycle the panel; a long RESETn assertion
    // gives it time to drop whatever state wedged it.
    if (_board != P4Board::Unknown) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << P4PanelCommonPins::LCD_RST,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        gpio_set_level((gpio_num_t)P4PanelCommonPins::LCD_RST, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    esp_restart();
}

// ════════════════════════════════════════════════════════════════════════════
//  Frames
// ════════════════════════════════════════════════════════════════════════════

uint16_t *chipguy_P486_or_JC8012_display::getDrawBuffer(uint8_t index)
{
    if (index >= 2) return nullptr;
    return _direct ? getFramebuffer(index) : _draw_buffers[index];
}

uint16_t *chipguy_P486_or_JC8012_display::getFramebuffer(uint8_t index)
{
    if (!_panel_handle || index >= NUM_FRAMEBUFFERS) return nullptr;
    void *fb_addrs[NUM_FRAMEBUFFERS] = {nullptr, nullptr};
    esp_err_t ret = chipguy_lcd_dpi_panel_get_frame_buffer(_panel_handle, NUM_FRAMEBUFFERS,
                                                           &fb_addrs[0], &fb_addrs[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get framebuffer: %s", esp_err_to_name(ret));
        return nullptr;
    }
    return (uint16_t *)fb_addrs[index];
}

void chipguy_P486_or_JC8012_display::setActiveFramebuffer(uint8_t index, bool wait_for_vsync)
{
    if (_panel_handle) chipguy_lcd_dpi_panel_set_active_fb(_panel_handle, index, wait_for_vsync);
}

void chipguy_P486_or_JC8012_display::present(const void *pixelmap)
{
    uint8_t buf_index = (pixelmap == (const void *)getDrawBuffer(0)) ? 0 : 1;
    if (_direct) {
        // LVGL rendered into the DMA framebuffer itself: push the CPU's writes
        // out to PSRAM and show it.
        esp_cache_msync((void *)pixelmap, framebufferSize(), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        setActiveFramebuffer(buf_index, false);
    } else {
        _flip(buf_index, _target_fb_index);
        _target_fb_index ^= 1;
    }
}

void chipguy_P486_or_JC8012_display::_ppaTaskFunc(void *arg)
{
    auto *self = (chipguy_P486_or_JC8012_display *)arg;
    const size_t fb_size = (size_t)self->_native_w * self->_native_h * sizeof(uint16_t);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        auto &w = self->_ppa_work;
        uint16_t *src = self->_draw_buffers[w.draw_buf_index];
        uint16_t *target_fb = self->getFramebuffer(w.target_fb_index);

        // Scale and rotate the canvas into its region of the native
        // framebuffer.  The area outside the region keeps the black it was
        // cleared to at begin().
        ppa_srm_oper_config_t srm_config = {
            .in = {
                .buffer = (const void *)src,
                .pic_w = self->_draw_w,
                .pic_h = self->_draw_h,
                .block_w = self->_draw_w,
                .block_h = self->_draw_h,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = (void *)target_fb,
                .buffer_size = fb_size,
                .pic_w = self->_native_w,
                .pic_h = self->_native_h,
                .block_offset_x = self->_region_x,
                .block_offset_y = self->_region_y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = ppa_angle_for(self->_rotation),
            .scale_x = self->_scale,
            .scale_y = self->_scale,
            .rgb_swap = 0,
            .byte_swap = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        esp_err_t ret = ppa_do_scale_rotate_mirror(self->_ppa_client, &srm_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PPA scale/rotate failed: %s", esp_err_to_name(ret));
        } else {
            esp_cache_msync(target_fb, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            self->setActiveFramebuffer(w.target_fb_index, false);
        }
        xSemaphoreGive(self->_ppa_done_sem);
    }
}

void chipguy_P486_or_JC8012_display::_flip(uint8_t draw_buf_index, uint8_t target_fb_index)
{
    if (!_ppa_client || draw_buf_index >= 2) return;
    uint16_t *src = _draw_buffers[draw_buf_index];
    if (!src || !getFramebuffer(target_fb_index)) return;

    if (!_ppa_task_handle) {
        _ppa_done_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(_ppa_done_sem);
        xTaskCreatePinnedToCore(_ppaTaskFunc, "ppa_flip", 4096, this, 5, &_ppa_task_handle, 1);
    }

    // Wait for the previous PPA operation, flush the source out of the CPU
    // cache, and wake the task.
    xSemaphoreTake(_ppa_done_sem, portMAX_DELAY);
    esp_cache_msync(src, framebufferSize(), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    _ppa_work = { draw_buf_index, target_fb_index };
    xTaskNotifyGive(_ppa_task_handle);
}

bool chipguy_P486_or_JC8012_display::waitVsync(uint32_t timeout_ms)
{
    return chipguy_lcd_dpi_panel_wait_vsync(timeout_ms);
}

uint8_t chipguy_P486_or_JC8012_display::getNumFramebuffers()
{
    return _panel_handle ? chipguy_lcd_dpi_panel_get_num_fbs(_panel_handle) : 0;
}

uint32_t chipguy_P486_or_JC8012_display::getFrameCount()    { return chipguy_lcd_dpi_panel_get_frame_count(); }
uint32_t chipguy_P486_or_JC8012_display::getUnderrunCount() { return chipguy_lcd_dpi_panel_get_underrun_count(); }

// ════════════════════════════════════════════════════════════════════════════
//  Touch
// ════════════════════════════════════════════════════════════════════════════

bool chipguy_P486_or_JC8012_display::_rawTouch(uint16_t &x, uint16_t &y)
{
    if (!_touch_ok) return false;
    return (_board == P4Board::JC8012) ? _gsl.getTouch(&x, &y) : _gt911.getTouch(&x, &y);
}

bool chipguy_P486_or_JC8012_display::getTouch(int32_t &x, int32_t &y)
{
    uint16_t rx, ry;
    return _rawTouch(rx, ry) && mapTouch(rx, ry, x, y);
}

// Both controllers report in NATIVE panel coordinates.  Undo what the PPA did,
// in reverse order: the region offset, then the rotation, then the scale.
// Dividing by the region extent and multiplying by the canvas extent does the
// unscale exactly in integer maths, since the region IS the canvas x scale
// (and at scale 1 it reduces to the plain inverse rotation).
bool chipguy_P486_or_JC8012_display::mapTouch(uint16_t rawX, uint16_t rawY, int32_t &lx, int32_t &ly) const
{
    int32_t ax = (int32_t)rawX - _region_x;
    int32_t ay = (int32_t)rawY - _region_y;
    if (ax < 0 || ax >= _region_w || ay < 0 || ay >= _region_h) return false;

    int32_t x, y;
    switch (_rotation) {
        case 1:  // picture 90 clockwise = PPA 270 (CCW)
            x = ay * _draw_w / _region_h;
            y = (_region_w - 1 - ax) * _draw_h / _region_w;
            break;
        case 2:  // PPA 180
            x = (_region_w - 1 - ax) * _draw_w / _region_w;
            y = (_region_h - 1 - ay) * _draw_h / _region_h;
            break;
        case 3:  // picture 270 clockwise = PPA 90 (CCW)
            x = (_region_h - 1 - ay) * _draw_w / _region_h;
            y = ax * _draw_h / _region_w;
            break;
        default:
            x = ax * _draw_w / _region_w;
            y = ay * _draw_h / _region_h;
            break;
    }
    if (x < 0 || x >= _draw_w || y < 0 || y >= _draw_h) return false;
    lx = x;
    ly = y;
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  Backlight, radio
// ════════════════════════════════════════════════════════════════════════════

void chipguy_P486_or_JC8012_display::setBacklight(int percentage)
{
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    _backlight_pct = percentage;

    if (_board == P4Board::JC8012) {
        // Active HIGH: higher duty = brighter.
        analogWrite(JC8012Pins::LCD_BACKLIGHT, percentage * 255 / 100);
    } else if (_board == P4Board::P486) {
        // Active LOW.  The bottom 40% of the PWM range gives no visible light,
        // so 1-100% maps across the usable upper 60% (duty 153..0).
        int duty = percentage == 0 ? 255 : (100 - percentage) * 153 / 99;
        analogWrite(P486Pins::LCD_BACKLIGHT, duty);
    }
}

void chipguy_P486_or_JC8012_display::setRadioPower(bool on)
{
    if (on) {
        pinMode(P4PanelCommonPins::C6_RESET, INPUT);      // the board's pull-up runs the C6
    } else {
        pinMode(P4PanelCommonPins::C6_RESET, OUTPUT);
        digitalWrite(P4PanelCommonPins::C6_RESET, LOW);   // held in reset
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Screen off / sleep
// ════════════════════════════════════════════════════════════════════════════

void chipguy_P486_or_JC8012_display::sleep()
{
    if (_asleep || !_io_handle) return;
    int keep = _backlight_pct;
    setBacklight(0);
    _backlight_pct = keep;           // wake() restores the level from before

    if (_board == P4Board::P486) {   // backlight only on this panel
        _asleep = true;
        return;
    }

    // Let a flip in flight finish so the panel isn't mid-update.
    if (_ppa_done_sem) {
        xSemaphoreTake(_ppa_done_sem, portMAX_DELAY);
        xSemaphoreGive(_ppa_done_sem);
    }
    esp_lcd_panel_io_tx_param(_io_handle, 0x28, NULL, 0);   // Display Off
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_lcd_panel_io_tx_param(_io_handle, 0x10, NULL, 0);   // Sleep In
    vTaskDelay(pdMS_TO_TICKS(120));                          // JD9365 needs 120 ms

    // Stop the video stream at a frame boundary, so the chip can light-sleep
    // without cutting the DSI DMA off mid-transfer (which hangs the bus and
    // trips the hardware watchdog).
    esp_err_t err = chipguy_lcd_dpi_panel_pause(_panel_handle, 100);
    if (err != ESP_OK) ESP_LOGE(TAG, "sleep(): stream pause failed: %s", esp_err_to_name(err));
    _asleep = true;
}

void chipguy_P486_or_JC8012_display::wake()
{
    if (!_asleep || !_io_handle) return;
    if (_board == P4Board::JC8012) {
        chipguy_lcd_dpi_panel_resume(_panel_handle);
        esp_lcd_panel_io_tx_param(_io_handle, 0x11, NULL, 0);   // Sleep Out
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_io_tx_param(_io_handle, 0x29, NULL, 0);   // Display On
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    _asleep = false;
    setBacklight(_backlight_pct);
}

chipguy_P486_or_JC8012_display::WakeReason chipguy_P486_or_JC8012_display::sleepUntilSwipe(uint32_t checkMs)
{
    return _board == P4Board::JC8012 ? _jcLightSleep(1, checkMs) : _p486ScreenOffUntilTouch();
}

chipguy_P486_or_JC8012_display::WakeReason chipguy_P486_or_JC8012_display::sleepUntilTap()
{
    return _board == P4Board::JC8012 ? _jcLightSleep(2, 0) : _p486ScreenOffUntilTouch();
}

chipguy_P486_or_JC8012_display::WakeReason chipguy_P486_or_JC8012_display::sleepUntilButton()
{
    return _board == P4Board::JC8012 ? _jcLightSleep(0, 0) : _p486ScreenOffUntilTouch();
}

// Wait for the finger that asked for sleep (if any) to lift, so it doesn't
// immediately count as the wake.  Raw touch, so a finger on the black border
// around a scaled image counts too.
void chipguy_P486_or_JC8012_display::_waitForLift()
{
    if (!_touch_ok) return;
    uint32_t quietSince = millis();
    while (millis() - quietSince < 300) {
        uint16_t x, y;
        if (_rawTouch(x, y)) quietSince = millis();
        delay(15);
    }
}

chipguy_P486_or_JC8012_display::WakeReason chipguy_P486_or_JC8012_display::_p486ScreenOffUntilTouch()
{
    _waitForLift();
    sleep();
    if (_touch_ok) {
        uint16_t x, y;
        while (!_rawTouch(x, y)) delay(20);
    } else {
        ESP_LOGW(TAG, "screen-off: no touch controller to wake on; returning at once");
    }
    wake();
    return WAKE_TAP;
}

// ── JC8012 low-power sleep (from chipguy_JC8012P4A1C_display) ───────────────
// Timings measured on the board (see chipguy_JC8012P4A1C_power's
// README-engineering): after touch resume(false) the controller calibrates,
// and a moving finger is first reported ~118 ms later.  Sleep through the
// first part, poll the rest.
static constexpr uint32_t SWIPE_QUIET_MS  = 100;
static constexpr uint32_t SWIPE_DETECT_MS = 170;

static bool bootHeld()
{
    return gpio_get_level((gpio_num_t)P4PanelCommonPins::BOOT_BUTTON) == 0;
}

// mode: 0 = button only, 1 = swipe (duty-cycled touch), 2 = tap (touch armed)
chipguy_P486_or_JC8012_display::WakeReason chipguy_P486_or_JC8012_display::_jcLightSleep(uint8_t mode, uint32_t checkMs)
{
    const gpio_num_t BOOT = (gpio_num_t)P4PanelCommonPins::BOOT_BUTTON;
    const gpio_num_t TINT = (gpio_num_t)JC8012Pins::TOUCH_INT;
    if (!_touch_ok) mode = 0;

    _waitForLift();
    sleep();   // backlight off, Display Off + Sleep In, video stream paused

    // Keep these pins as driven during light sleep: the C6's reset (whatever
    // setRadioPower() chose), the touch reset/INT, the panel reset, BOOT.
    gpio_sleep_sel_dis((gpio_num_t)P4PanelCommonPins::C6_RESET);
    gpio_sleep_sel_dis((gpio_num_t)JC8012Pins::TOUCH_RST);
    gpio_sleep_sel_dis(TINT);
    gpio_sleep_sel_dis((gpio_num_t)P4PanelCommonPins::LCD_RST);
    gpio_sleep_sel_dis(BOOT);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    gpio_wakeup_enable(BOOT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    WakeReason why = WAKE_BUTTON;
    if (mode == 2) {
        // Touch armed: its INT line idles high and goes low on a touch.
        int idle = gpio_get_level(TINT);
        gpio_wakeup_enable(TINT, idle ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
        esp_light_sleep_start();
        gpio_wakeup_disable(TINT);
        why = bootHeld() ? WAKE_BUTTON : WAKE_TAP;
    } else if (mode == 1) {
        for (;;) {
            _gsl.suspend();                                     // touch in reset
            esp_sleep_enable_timer_wakeup((uint64_t)checkMs * 1000ULL);
            esp_light_sleep_start();
            _gsl.resume(false);                                 // ~80 ms, no reload
            if (bootHeld()) break;
            uint32_t t0 = millis();
            esp_sleep_enable_timer_wakeup((uint64_t)SWIPE_QUIET_MS * 1000ULL);
            esp_light_sleep_start();                            // through calibration
            if (bootHeld()) break;
            bool seen = false;
            while (millis() - t0 < SWIPE_DETECT_MS) {
                uint16_t x, y;
                if (_gsl.getTouch(&x, &y)) { seen = true; break; }
                delay(5);
            }
            if (seen) { why = WAKE_SWIPE; break; }
        }
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    } else {
        if (_touch_ok) _gsl.suspend();
        esp_light_sleep_start();
        if (_touch_ok) _gsl.resume(false);
    }

    gpio_wakeup_disable(BOOT);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    wake();
    return why;
}

#endif // ARDUINO_ESP32P4_DEV
