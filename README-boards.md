# The two boards, for developers

What a sketch built on `chipguy_P486_or_JC8012_display` needs to know about the hardware
underneath: which pins the library owns, which are dangerous to drive on the wrong
board, and how touch, sleep, serial output and the network differ between the two.
For *how* any of this was worked out, see [README-engineering.md](README-engineering.md).

| | JC8012P4A1C | P486 Panel |
| --- | --- | --- |
| Panel | 800×1280 JD9365, 2-lane MIPI-DSI | 720×720 ST7703, 2-lane MIPI-DSI |
| Touch | GSL3680 at I²C 0x40 | GT911 at I²C 0x5D (or 0x14) |
| Flash / PSRAM | 16 MB / 32 MB | 32 MB / 32 MB |
| Radio | ESP32-C6 over SDIO | ESP32-C6 over SDIO |
| Wired network | none | IP101 Ethernet (RMII) |
| Power | USB, Li-ion battery | 12–24 V or PoE |

---

## Which board am I on?

Call `chipguy_p4panel_detect()` anywhere. `display.begin()` calls it for you, and so
does a NetClient `configureEthernet()`. It returns `P4Board::JC8012` or `P4Board::P486`,
or `P4Board::Unknown`, in which case `begin()` refuses to touch any pin and returns
`false`.

- **First boot on a board:** the I²C bus is probed. The JC8012's real-time clock answers
  at 0x32; the P486's touch controller answers at 0x5D. The answer is **saved in NVS**.
- **Every later boot:** the saved answer is used and nothing is probed.
- **If I²C gives no clear answer:** the size of the flash chip decides (32 MB → P486,
  16 MB → JC8012). That answer is used for this boot only and is **not** saved, so the
  next boot probes again.

NVS survives serial and OTA uploads. An "Erase All Flash" upload clears it, and so does
`chipguy_p4panel_forget()`. To test one board's code path, `chipguy_p4panel_force(P4Board::P486)`
before anything else overrides detection for one boot (nothing is saved).
`chipguy_p4panel_detect_source()` says how the answer was reached (`Nvs`, `I2c`,
`Flash`, `Forced`).

Measured on the bench units: probing found 0x18, 0x32, 0x36, 0x40 on the JC8012 and
0x18, 0x40, 0x5D on the P486. Both identified correctly by I²C.
**TODO (bench):** confirm that the P486's GT911 answers after a true power-on (only an
EN-pin reset has been tested); if it doesn't, the flash-size fallback still identifies
the board.

## Pins: what the library owns, what to avoid

The library touches **only GPIO7/8 (I²C)** until it knows the board. After that it owns:

| GPIO | JC8012P4A1C | P486 Panel |
| --- | --- | --- |
| 7 / 8 | I²C SDA / SCL (shared bus) | I²C SDA / SCL (shared bus) |
| 27 | panel reset | panel reset |
| 23 | **backlight**, active HIGH | **touch reset** |
| 26 | *(WS2812 LED — not used by the library)* | **backlight**, active LOW |
| 21 / 22 | touch INT / reset | *(free on the header)* |
| 35 | BOOT button, read during sleep | *(Ethernet TXD1 — not used by the library)* |
| 54 | ESP32-C6 reset, only if you call `setRadioPower()` | same |

Beyond those, the same GPIO is something different on each board. Drive these only
after checking `display.board()`:

| GPIO | JC8012P4A1C | P486 Panel |
| --- | --- | --- |
| 26 | RGB LED | backlight |
| 28–31 | FPC4 expansion header | Ethernet RXD0/RXD1/CRS_DV/MDC |
| 34, 35 | FPC4 header / BOOT | Ethernet TXD0, TXD1 (35 is also BOOT) |
| 49–51 | — | Ethernet TX_EN, REF_CLK, PHY reset |
| 52 | battery-sense ADC | Ethernet MDIO |

Pin constants are in `p4panel_pins.h` (`JC8012Pins::`, `P486Pins::`,
`P4PanelCommonPins::`).

## The I²C bus

One bus on both boards, GPIO7/8, 400 kHz, owned by the library. Reach it with
`p4panel_wire()`, a mutex-protected `ThreadsafeWire`. Every transaction locks itself,
so it is safe from any task:

```cpp
if (p4panel_wire().probe(0x44)) { /* a sensor answers */ }
p4panel_wire().readRegister(0x44, 0x00, buf, 2);
```

Never `Wire.begin()` on other pins: `::Wire` *is* this bus. What else is on it:

| Address | JC8012P4A1C | P486 Panel |
| --- | --- | --- |
| 0x18 | ES8311 codec | ES8311 codec |
| 0x32 | RX8025T RTC | — |
| 0x36 | camera (when fitted) | — |
| 0x40 | GSL3680 touch | ES7210 mic ADC |
| 0x5D | — | GT911 touch |

## Touch

`display.getTouch(x, y)` returns the first contact already mapped to canvas coordinates
— through rotation, and through the scaling of the 480 resolutions. On a scaled JC8012
picture, a touch in the black border around it reports *not touched*.

**Steadiness filter (JC8012 only), off by default.** `display.setTouchFilter(true)`
turns on a 7-sample median and phantom-jump rejection in the GSL3680 driver. The
panel occasionally reports a contact that jumps to the edge of the screen and sits
there; with the filter on, that can't drag a slider to its extreme. The cost is about
50 ms of lag, and a finger moving very fast is cut short. Turn it on for a home-control
UI with short gestures and volume-style sliders; leave it off otherwise. A short
dropout debounce applies either way. The P486's GT911 has no filter and hasn't needed one.

## Screen off and sleep

| | JC8012P4A1C | P486 Panel |
| --- | --- | --- |
| `sleepUntilSwipe()` | light sleep, ~16 mA; swipe or BOOT wakes | backlight off; touch wakes |
| `sleepUntilTap()` | light sleep, ~45 mA; touch or BOOT wakes | backlight off; touch wakes |
| `sleepUntilButton()` | light sleep, ~10 mA; BOOT wakes | backlight off; touch wakes |
| `sleep()` / `wake()` | panel Sleep In + video paused | backlight only |

JC8012 currents are measured on battery with the radio held off by
`display.setRadioPower(false)`; add ~22 mA if the C6 is running. A finger *resting* on
the JC8012 isn't seen by `sleepUntilSwipe()` — it has to move.
`display.supportsLowPowerSleep()` is `true` on the JC8012 only. The P486 is mains- or
PoE-powered, and light sleep isn't implemented for its panel.

## The P486's warm-restart recovery

After a soft reset (`ESP.restart()`, OTA, panic, software watchdog), the P486's panel
can come up black. On such a boot, `display.begin()` brings the panel up and then
forces a deeper reset (the Super Watchdog). The boot after that runs normally. So:

- A soft restart on a P486 costs one extra ~1 s boot. A power-on costs nothing.
- **Anything `setup()` does before `lv_setup.begin()` runs twice.** Start the network
  after it.
- It can't loop: the reset reason and an NVS flag both allow one promotion per soft
  reset. Measured on the bench unit: 8 soft restarts, 8 promotions, 8 clean boots.
- `display.setWarmBootQuiesce(false)` before `begin()` turns it off.

The JC8012 has its own recovery: a failed bring-up holds the panel in reset and reboots,
up to three times in a row, then continues with touch disabled.

## USB-C ports and serial output

Each board has one USB-C port wired to a USB-to-serial chip (a CH34x on both bench
units) and one or two wired straight to the ESP32-P4.

- **`Serial0` is UART0** (GPIO37/38) and always comes out of the **USB-to-serial port**.
  Upload through that port too.
- **"USB CDC On Boot"** only decides what `Serial` means. *Disabled* (recommended here):
  `Serial` is the same UART as `Serial0`, so libraries that print to `Serial` — the I²C
  bus's stall reports, for one — show up on the same port. *Enabled*: `Serial` is the
  ESP32-P4's own USB port.

## Network

Both boards have Wi-Fi from an ESP32-C6; the core's `WiFi` works on both with no pin
setup. Only the P486 has Ethernet. `display.hasEthernet()` says which.

With `chipguy_MQTT_netclient`, include `chipguy_MQTT_netclient_P486Panel.h` **and** set
`config.wifiSSID` / `wifiPassword`. Its `configureEthernet()` asks this library (through
a weak hook, `chipguy_netclient_ethernet_absent()`) whether there is Ethernet. On a P486
it fills in the Ethernet settings; on a JC8012 it does nothing, so NetClient uses Wi-Fi
and never touches GPIO28–35/49–52. Measured: the same LvglMqttStatus image reported
"Ethernet mode" on the P486 and "WiFi mode" on the JC8012.

## Build settings

- **Flash Size 16MB** — the JC8012's size. The lv_setup header refuses any other (define
  `CHIPGUY_ALLOW_ANY_FLASH_SIZE` to override, e.g. for a P486-only build).
- **Partition Scheme Custom** with the examples' `partitions.csv`: two 7.875 MB OTA app
  slots, 128 KB filesystem, coredump.
- **PSRAM Enabled.** Two framebuffers plus two draw buffers come to 8 MB on a native
  JC8012, 2 MB on a direct-mode P486.

## What isn't covered here

Sound, the TF card slot, battery, relays, RS485 and the expansion headers are different
on the two boards. A sketch using them on both needs per-board code (branch on
`display.board()`). The single-board libraries document each:
`chipguy_JC8012P4A1C_board` / `_power`, and `chipguy_P486Panel_display` (its
`README-board.md`, `README-sound.md`, `README-sdcard.md`, `README-power.md`) /
`chipguy_P486Panel_relays`.
