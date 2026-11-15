/*
 * lv_setup.hpp - The bridge between LVGL and your display + touch hardware.
 *
 * Right now it just pulls in the ready-made setup that ships inside the
 * chipguy_P486_or_JC8012_display library, so you don't have to maintain a copy:
 *
 *   - It creates a global `display`     (the driver for whichever board this
 *                                        boots on: JC8012P4A1C or P486 Panel)
 *   - It creates a global `lv_setup`    (an lv_setup_class instance)
 *   - lv_setup.begin(jc8012, p486)      works out the board and initializes
 *                                        display + touch + LVGL with the
 *                                        matching {rotation, resolution}
 *
 * --------------------------------------------------------------------------
 *  CUSTOMIZING HOW LVGL IS WIRED UP
 *  Open the real implementation at:
 *
 *      <Arduino libraries>/chipguy_P486_or_JC8012_display/src/lv_setup_P486_or_JC8012.hpp
 *
 *  copy its entire contents over the #include line below, and edit freely.
 *  Nothing else in the sketch needs to change.
 * --------------------------------------------------------------------------
 *
 * Include this file after lv_conf.h and lvgl.h in your sketch.
 *
 * For reference, the setup class this pulls in looks like:
 *
 *   class lv_setup_class {
 *   public:
 *       // One {rotation, resolution} per board; the matching one is used.
 *       //   rotation:   0/1/2/3, or 0/90/180/270
 *       //   resolution: P4Res::Native, P4Res::Square480, P4Res::Full480
 *       void begin(P4Choice jc8012, P4Choice p486);
 *       void begin(P4Choice both = {});     // the same choice on both boards
 *       chipguy_P486_or_JC8012_display &displayDriver();
 *   };
 *
 *   // After begin(), the global `display` reports what actually came up:
 *   //   display.board()        P4Board::JC8012 or P4Board::P486
 *   //   display.width(), display.height()   the canvas LVGL renders at
 *   //   display.hasEthernet()  true on the P486
 *
 * Both boards have capacitive touch — no calibration is required.
 */

#pragma once

#include <lv_setup_P486_or_JC8012.hpp>
