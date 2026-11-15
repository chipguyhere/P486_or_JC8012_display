# chipguy_P486_or_JC8012_display

One display library for **two** touchscreen boards built on the ESP32-P4 chip:

- the **Guition JC8012P4A1C** — a 10.1-inch screen, 800×1280 pixels, with Wi-Fi and a
  battery connector
- the **Waveshare ESP32-P4-86-Panel-ETH-2RO** ("P486") — a square 4-inch wall panel,
  720×720 pixels, with Wi-Fi and an Ethernet jack

You build your sketch **once**, and the same file runs on either board. When it starts,
the library works out which board it is on and drives that screen and its touch panel.
You write an **LVGL 9** app (LVGL is the graphics library that draws the buttons,
sliders and text) and the library does the rest.

Use it for apps that are mostly a screen: a control panel, a dashboard, a status
display. If you only ever use one of the two boards, that board's own library has more
board features: [`chipguy_JC8012P4A1C_display`](https://github.com/chipguyhere/JC8012P4A1C_display) or
[`chipguy_P486Panel_display`](https://github.com/chipguyhere/P486Panel_display).

---

## Installation

This library is distributed by downloading the code as a ZIP from GitHub — there is no
Arduino Library Manager entry. To install:

1. On the GitHub project page, click **Code ▸ Download ZIP**.
2. Unzip it, and move the folder into your Arduino **libraries** folder:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Restart Arduino IDE so the examples appear in the File menu.

**Once per board:** run the **[firmware installer](https://chipguyhere.github.io/esp-launchpad/minimal-launchpad/?flashConfigURL=https://raw.githubusercontent.com/chipguyhere/ESP32-P4-C6-firmware-installer/main/launchpad.toml)**: one click in Chrome or Edge,
no IDE needed. On a JC8012 it installs the touch controller's firmware, which this library
doesn't include (until then the screen works but touch doesn't, and the serial log says
why); on both boards it brings the Wi-Fi chip's firmware up to date.
[How and why](https://github.com/chipguyhere/ESP32-P4-C6-firmware-installer/blob/main/README.md).

Nothing else from chipguy is needed. **Don't** install it next to the single-board
libraries above *in the same sketch* — they contain copies of the same driver code, and
the sketch won't link.

### Dependency: LVGL 9

Install **LVGL** from the Arduino **Library Manager** (search "lvgl"). Each example folder
has its own `lv_conf.h` settings file; keep it next to the `.ino`.

### Arduino IDE board settings

The settings are **the same for both boards**. In **Tools**, set:

- **Board:** ESP32P4 Dev Module
- **PSRAM:** Enabled *(required)*
- **Flash Size:** 16MB (128Mb) *(required — even on the P486, which has 32 MB; see below)*
- **Partition Scheme:** Custom *(uses the `partitions.csv` in the example folder)*
- **USB CDC On Boot:** Disabled

If PSRAM or Flash Size is wrong, the build stops with a message that says which. Flash
Size has to be 16MB because the JC8012 has 16 MB of memory for programs; a program built
for 32 MB will not start on it. On the P486 a 16MB build runs fine and simply leaves half
its memory unused.

**Which USB-C port.** Each board has a USB-C port wired to a USB-to-serial chip, plus
one or two that go straight to the ESP32-P4. Plug into the **USB-to-serial** one: you
upload through it, and the examples' messages (`Serial0`) come out of it. "USB CDC On
Boot" doesn't change that; it only decides what the name `Serial` means.
[README-boards.md](README-boards.md#usb-c-ports-and-serial-output) says which port is
which.

---

## Choosing a resolution

`lv_setup.begin()` takes **one choice per board**: the JC8012's first, then the P486's.
Each choice is a rotation and a resolution:

```cpp
lv_setup.begin({0, P4Res::Native},      // on a JC8012
               {0, P4Res::Native});     // on a P486
```

**Rotation** is `0`, `90`, `180` or `270` (or `0`–`3`). It turns the picture, and touch
turns with it. `90` turns the picture clockwise, the same as most Arduino display
libraries. So if you mount the board turned a quarter-turn **clockwise**, use `270`; a
quarter-turn **counter-clockwise**, use `90`.

**Resolution** is one of:

| Choice | On the JC8012 | On the P486 |
| --- | --- | --- |
| `P4Res::Native` | 800×1280 (1280×800 turned sideways) | 720×720 |
| `P4Res::Square480` | 480×480, enlarged to a square in the middle of the screen | 480×480, enlarged to fill the screen |
| `P4Res::Full480` | 480×768 (768×480 sideways), enlarged to nearly fill the screen | same as `Square480` |

"Native" uses every pixel. The **480** choices draw a smaller picture and let the chip's
graphics hardware enlarge it. That is free (it costs no speed), and it means **one
480×480 design runs on both boards** — handy for UIs made in SquareLine Studio.

After `begin()`, ask the library what you got: `display.width()`, `display.height()`,
`display.boardName()` (`"JC8012"` or `"P486"`), and `display.hasEthernet()`. Lay your
screen out from `width()` and `height()` instead of fixed numbers, and it will look right
on both boards. The examples all do this.

How the board is recognized, and why it is remembered:
[README-engineering.md](README-engineering.md#board-detection).

---

## The examples

| Example | What it shows |
| --- | --- |
| **LvglClaudeCodeStub** | The starting point: a "Hello, world!" screen that follows your finger. Replace `ui_init()` in `ui.cpp` with your own UI. Good to hand to an AI assistant. |
| **LvglCalculator** | A finished calculator whose keypad sizes itself to the screen. |
| **LvglBouncingBalls** | Animation plus two sliders, laid out from the screen size. |
| **Lvgl93SquarelineLauncher** | A SquareLine Studio design at 480×480 on both boards. Export your own SquareLine UI into its `src` folder. |
| **LvglSleepOnIdle** | Turns the screen off when nobody touches it. On the JC8012 the chip sleeps too, for battery life. |
| **LvglMqttStatus** | Connects to an MQTT server with `chipguy_MQTT_netclient`: over Ethernet on the P486, over Wi-Fi on the JC8012, from the same build. |

---

## The `lv_setup` API

Every example starts the same way:

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial0.begin(115200);
    lv_setup.begin({0, P4Res::Native}, {0, P4Res::Native});
    ui_init();                 // your UI
}

void loop() {
    lv_timer_handler();        // let LVGL draw and read touch
    delay(5);
}
```

`lv_setup.hpp` creates two things for you:

- **`lv_setup`** — starts the screen, touch and LVGL. Also `lv_setup.onTouch` (a function
  called on every touch, e.g. to reset a screen-off timer) and
  `lv_setup.sleepUntilTap()` and friends (screen off until touched).
- **`display`** — the driver. `display.setBacklight(0..100)`, the size and board
  questions above, and `display.setTouchFilter(true)` for a steadier touch on the JC8012
  (see [README-boards.md](README-boards.md#touch)).

**One thing to know about the P486:** after a *software* restart (an update, a crash, or
`ESP.restart()`), its screen can come up black. The library fixes that by restarting once
more, more thoroughly. So anything your `setup()` does *before* `lv_setup.begin()` can
happen twice. Put such things after it.

---

## The rest of the boards

This library drives the screen and touch, and can tell a networking library whether
there is Ethernet. The other parts of each board — sound, the memory-card slot, relays,
battery, buttons — differ between the two boards, and are covered by each board's own
libraries.

- **Network:** both boards have Wi-Fi. Only the P486 has Ethernet. With
  `chipguy_MQTT_netclient`, include its P486 Ethernet header and also give your Wi-Fi
  details: the P486 uses the cable, and the JC8012 uses Wi-Fi automatically. See the
  **LvglMqttStatus** example.
- **Pins:** most pins mean different things on the two boards, and a few are risky to
  use on the wrong one. [README-boards.md](README-boards.md) lists what the library uses
  and what to avoid.

## Where to go next

- [README-boards.md](README-boards.md) — for developers: pins the library owns on each
  board, the shared I2C bus, touch behavior, screen-off and sleep, USB ports, Ethernet.
- [README-engineering.md](README-engineering.md) — for engineers: how detection works and
  why it is stored, the scale-and-rotate geometry, where each piece of driver code came
  from, and what was measured on the bench.

## License

MIT for the library's own code, including both touch drivers; see [LICENSE](LICENSE).
The vendored ESP-IDF panel drivers (`esp_lcd_jd9365.*`, `esp_lcd_panel_dpi_bb.*`) are
under the **Apache License, Version 2.0**; see [LICENSE-Apache-2.0.txt](LICENSE-Apache-2.0.txt).
Nothing here is GPL: the JC8012 touch controller's firmware, which is Silead's and
GPL-licensed, is kept out of this library and put on the board by the
[firmware installer](https://github.com/chipguyhere/ESP32-P4-C6-firmware-installer/blob/main/README.md).
