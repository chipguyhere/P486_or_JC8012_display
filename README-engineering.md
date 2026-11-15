# Engineering notes

How `chipguy_P486_or_JC8012_display` is put together, and why. Sectioned to follow
[README-boards.md](README-boards.md), which states the facts these notes derive.

## Provenance

Everything here was merged from five existing libraries. Each board's bring-up,
including its recovery logic, is kept as it was, line for line where possible:

| Piece | From | Changes |
| --- | --- | --- |
| JC8012 bring-up, reset timing, retry budget, light sleep | `chipguy_JC8012P4A1C_display` | pin names from `p4panel_pins.h`; no `JC8012P4A1C_Board` |
| 480-wide geometry (Square / Full, x1.625, regions) | `chipguy_JC8012P4A1C_480display` | folded into one geometry table |
| P486 bring-up, recovery pulse, Super-WDT promotion | `chipguy_P486Panel_display` | same NVS namespace (`cgp486`), so the loop guard holds across a reflash between the two libraries |
| P486 480×480 x1.5 path | `chipguy_P486Panel_480display` | folded into the geometry table |
| GT911 driver | `chipguy_P486Panel_touch` | ported from its own ESP-IDF I²C master bus to the shared `ThreadsafeWire`; same reset, retry and read sequence |
| `esp_lcd_panel_dpi_bb.*` | the JC8012 copy | a strict superset of the P486 copy (adds `chipguy_lcd_dpi_panel_pause/resume`); the P486 path doesn't call those |
| `esp_lcd_jd9365.*`, `gsl3680_touch.*` | `chipguy_JC8012P4A1C_display` | GSL3680 driver: own address constant and this library's `ThreadsafeWire`; otherwise identical. It loads the controller firmware from NVS (put there by `chipguy_ESP32-P4-C6-firmware-installer`) and runs no host tracking algorithm — see that library's engineering notes, "Touch: GSL3680" |
| `chipguy_ThreadsafeWire.*` | `chipguy_JC8012P4A1C_board` (vendored from `chipguy_Board`) | verbatim |

Linking the original libraries together instead was not an option. The two P486
libraries and the two JC8012 libraries each carry their own copy of
`esp_lcd_panel_dpi_bb.c` (and the JC8012 ones the `gsl3680_touch` class), with the same
external symbols. For the same reason this library must not share a sketch with any of them, or
with `chipguy_JC8012P4A1C_board` (which defines `ThreadsafeWire`).

## Board detection

### Why the I²C bus, and which addresses

Both boards put I²C on GPIO7/8, so probing there is the one bus operation that is safe
before the board is known. Every other candidate pin is something different on each
board (see the clash table in [README-boards.md](README-boards.md#pins-what-the-library-owns-what-to-avoid)).
The probes are address-only (START, address, STOP); no register is written.

| Address | JC8012P4A1C | P486 Panel | Useful? |
| --- | --- | --- | --- |
| 0x18 | ES8311 | ES8311 | no — on both |
| 0x40 | GSL3680 | ES7210 | no — on both |
| 0x32 | RX8025T RTC | — | **yes** |
| 0x5D / 0x14 | — | GT911 | **yes** |

The RTC is the better JC8012 signature: it runs from its CR1220 coin cell and has no
reset line, so nothing the previous firmware did can silence it. The GSL3680 answers
"only once out of reset" (per `JC8012P4A1C_Board.h`), and its address is taken anyway.
The GT911 latches 0x5D or 0x14 at reset depending on its INT pin, which the P486 leaves
unconnected, so both are probed. Rule: exactly one signature answering decides; neither
or both falls through to the flash size.

Bench (EN-pin reset, after previous firmware had run the touch controllers): JC8012 —
0x18, 0x32, 0x36, 0x40; P486 — 0x18, 0x40, 0x5D. **TODO (bench):** the GT911 with its
reset line floating after a true power-on.

### Why the flash size as fallback

`esp_flash_get_physical_size()` reads the chip's JEDEC ID, so it reports the part
fitted (16 MB on the JC8012, 32 MB on the P486), not the 16 MB the build was configured
for. It needs no GPIO at all. It is the fallback, not the primary, because it says
nothing about the panel or touch controller, and a board revision could ship with a
different part. When the two disagree, I²C wins and the disagreement is logged at
`ESP_LOGW`.

The fallback matters for one specific failure: a touch controller caught mid-transaction
by a watchdog reset can hold SDA low (both original drivers pulse the touch reset
"to make it let go of the bus" for exactly this reason). That kills every probe on the
bus, the RTC's included, until the controller is reset — and resetting it means driving
GPIO22 or GPIO23, which can't be done before the board is known.

### Why the answer is stored

Detection is a fact about the hardware, so after one good answer there is nothing to
gain by asking again, and two things to lose: the wedged-bus case above, and a future
revision whose flash or I²C population differs. So the first definite **I²C** answer is
written to NVS (namespace `cgp4panel`, key `board`, a `uint8_t` of `P4Board`). Every
later boot reads it and probes nothing.

A flash-size answer is deliberately **not** stored. It is the less reliable of the two,
and storing it would make a one-off wedged bus permanent. Using it for the current boot
only means the next boot gets another chance at an I²C answer.

NVS is untouched by esptool's normal upload, which writes the bootloader, partition
table, `otadata` and the app, and by OTA. The stored value goes away only with an
erase-all or `chipguy_p4panel_forget()`. A stale value is impossible in practice: the
NVS partition lives on the board's own flash.

### The Ethernet hook

`chipguy_MQTT_netclient_P486Panel.h` declares
`extern "C" bool chipguy_netclient_ethernet_absent() __attribute__((weak))` and returns
early from `configureEthernet()` when it exists and returns true. This library defines
it in `chipguy_P486_or_JC8012_display.cpp`, the same object file as `begin()`. That
placement matters: a weak *undefined* reference does not pull a member out of an
archive, so a definition sitting in an otherwise-unreferenced object could be dropped
if a library were ever linked as a `.a`. Any sketch using the display references that
object, so the hook always comes with it. `Unknown` counts as absent: driving the P486's
RMII pins on anything else is the failure to avoid.

## Geometry

One model covers all six board × resolution cases: a **canvas** (what LVGL renders), a
**region** of the native panel (where the PPA writes it), a **scale** and a
**rotation**.

| Board | Resolution | Canvas (rot 0/2) | Canvas (rot 1/3) | Region | Origin | Scale |
| --- | --- | --- | --- | --- | --- | --- |
| JC8012 | Native | 800×1280 | 1280×800 | 800×1280 | (0, 0) | 1 |
| JC8012 | Square480 | 480×480 | 480×480 | 780×780 | (10, 250) | 1.625 |
| JC8012 | Full480 | 480×768 | 768×480 | 780×1248 | (10, 16) | 1.625 |
| P486 | Native | 720×720 | 720×720 | 720×720 | (0, 0) | 1 |
| P486 | Square480 / Full480 | 480×480 | 480×480 | 720×720 | (0, 0) | 1.5 |

The PPA transposes its output at 90°/270°. That is why a landscape canvas lands on the
same portrait region as its portrait twin, and why the region never depends on
rotation. 1.625 = 26/16 and 1.5 = 24/16 are exact in the PPA's 4-bit fractional scale,
so 480 → 780, 768 → 1248 and 480 → 720 with no rounding.

**Direct mode.** P486 + Native + rotation 0 is the one case with nothing for the PPA to
do. LVGL's two DIRECT-mode buffers are then the two DPI framebuffers themselves, and
`present()` is a cache writeback plus a page flip. That saves 2 MB of PSRAM and a PPA
pass, as `chipguy_P486Panel_display` did. The JC8012 at Native/0 could do the same, but
its original driver always went through the PPA (asynchronously, on its own task, so
LVGL can start the next frame while the PPA writes this one), and that path is kept.

**PPA path.** Both boards use the JC8012's asynchronous flip: a pinned task (core 1,
priority 5) waits for a notification, runs a blocking `ppa_do_scale_rotate_mirror` into
the target framebuffer, invalidates it, and switches the DPI scan-out to it. The P486's
original rotate path ran synchronously inside the LVGL flush; moving it to the task
changes when the CPU is free, not what is drawn.

**Touch mapping.** Both controllers report native coordinates. `mapTouch()` subtracts
the region origin (rejecting the border), then applies the inverse rotation and the
inverse scale together, as `(region_extent − 1 − a) × canvas_extent / region_extent`
and its variants. At scale 1 that reduces to the plain inverse rotation both original
drivers used: P486 at PPA 90° gave `x = 719 − ty, y = tx`, and the general form gives the
same.

**Rotation direction.** The PPA's angles are counter-clockwise; the library's rotation
follows the usual Arduino display convention, where 1 (90) turns the picture clockwise.
`ppa_angle_for()` therefore maps 1 → PPA 270 and 3 → PPA 90, and `mapTouch()` cases 1
and 3 are swapped to match. Before 2026-10-29 the library passed 1 → PPA 90 directly,
so its 90 and 270 were the reverse of other Arduino libraries; a sketch written against
that version swaps its 90s and 270s to keep the same picture.

### Bench results

Soak test: each step one boot, 4 s of animation, then `ESP.restart()` into the next.

| Board | Steps | Frames / 4 s | Underruns | begin() |
| --- | --- | --- | --- | --- |
| JC8012 | Native ×4 rotations, Square480 rot 0/1, Full480 rot 0/3 | 208–210 (~52 fps) | 0 | ~1.8 s |
| P486 | the same eight | 267–268 (~67 fps) | 0 | ~1.0 s |

Touch-mapping corners were checked for every step against the expected rotation.
(These runs predate the rotation-direction fix above: their rotation 1 and 3 are PPA
90° and 270°, today's 3 and 1.) On
the P486 every soft restart produced exactly one `rst:0x12 (SUPER_WDT_RESET)` and then a
normal boot.

## Touch drivers

**GT911 on the shared bus.** The original opened I²C port 0 with
`i2c_new_master_bus()`. Here port 0 already belongs to Arduino `Wire` (through
`ThreadsafeWire`), because detection had to use it before anything else ran, so the
GT911's 16-bit register reads are done under a `ThreadsafeWire::Guard` with
`endTransmission(false)` + `requestFrom()` (a repeated start, as
`esp_lcd_panel_io_rx_param` did). Reset pulse, 210 ms settle, 20 × 250 ms identify
retries, and the status/clear-flag handshake are unchanged, as is the behavior when the
status register says "no new frame": report not touched.

**GSL3680 steadiness filter.** The original driver always ran a 7-read sliding median
and rejected any contact that moved more than 350 raw units between reads. It held that
contact for up to 6 reads, then dropped the touch. The thresholds were measured on a
10.1-inch panel: real finger travel per 16 ms read had a median of 7 units, a 99th
percentile of 180 and a maximum of 187, while phantom jumps were 445–545. The filter
suits a home-control panel, where a phantom could slam a volume slider to its extreme,
but it is not normal driver behavior, so it is **off by default**
(`setTouchFilter(true)` enables it). The 65 ms release debounce stays on either way:
it distinguishes a failed I²C read from a lifted finger, which is a correctness matter,
not a speed trade-off. The holds are timed rather than counted in reads, so a caller
that polls slowly doesn't stretch a tap into a drag.

## P486 warm-restart promotion

Unchanged from `chipguy_P486Panel_display`. The measurements behind it: a light sleep
with full DSI rebuild didn't recover a black panel, deep sleep didn't either, and RWDT
(`0x10`) recovered once and then failed twice. The EN pin always recovers. The Super
Watchdog (`0x12`, digital core + RTC domain) recovered 20 of 20 when fired with the DSI
link running and owned, and 0 of 11 after an orderly teardown, which is why it fires at
the bottom of `begin()` with no teardown. The RESETn recovery pulse (100 ms low, 100 ms
high, released to Hi-Z, 100 ms quiet) runs first, as in the original.

One ordering change: the original sketch-level glue brought up touch *after*
`display.begin()` returned; here `_beginP486()` starts the GT911 itself, still after the
promotion point, so a promoted boot never touches the GT911.

## Serial ports and CDC

On the ESP32 Arduino core, `Serial0` is always the `HardwareSerial` on UART0. The
"USB CDC On Boot" menu sets `ARDUINO_USB_CDC_ON_BOOT`, which only redirects the `Serial`
alias: to the P4's USB Serial/JTAG (native USB) when enabled, to `Serial0` when
disabled. UART0 is GPIO37/38 on both boards and goes to the USB-to-serial chip's port,
so `Serial0` output is unaffected by the setting. The examples choose *Disabled* so that
`Serial` and `Serial0` coincide; `ThreadsafeWire`'s stall reports print to `Serial`.

## Opportunities

- **TF card, sound, power/battery and board topic files.** Not written for this library:
  each board's libraries own them, and they differ completely between the two boards.
  A sketch that needs one of them on both boards would be the reason to add a
  per-board summary here.
- **P486 light sleep.** `sleep()` on the P486 only turns off the backlight. The JC8012's
  stream pause/resume would work on the same `dpi_bb` driver, but the ST7703's Sleep
  In/Out behavior around the black-panel fault hasn't been measured.
- **GT911 "no new frame".** Reporting *not touched* when the status register has no new
  data can produce a one-poll release during a long press, if LVGL polls faster than the
  controller's report rate. That is unchanged from the original driver and hasn't been
  seen to matter; holding the last state for a frame or two would fix it.
