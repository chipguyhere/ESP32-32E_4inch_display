/*
 * XPT2046 resistive touch driver for the ESP32-32E 4.0" touch LCD.
 *
 * Unlike boards where the touch panel has its own SPI pins, the XPT2046 on the
 * ESP32-32E board SHARES the display's SPI bus (SCLK 14 / MOSI 13 / MISO 12).  So this
 * driver does NOT bit-bang; instead it adds the XPT2046 to the display's
 * ESP-IDF SPI bus as a second device with its own chip-select and a slow clock,
 * and lets the SPI master driver arbitrate the bus between display and touch.
 *
 * Board wiring (fixed on the ESP32-32E board):
 *   Shared SCLK 14   MOSI 13   MISO 12
 *   Touch CS 33   Touch IRQ 36 (PENIRQ, active LOW while touched)
 *
 * Because the bus is shared, call this driver's begin() AFTER the display's
 * begin() (which initializes the bus), and make sure no display DMA is in
 * flight when you read touch — the lv_setup glue drains it with waitFlush()
 * before each read.
 *
 * Resistive panels vary unit-to-unit, so raw ADC readings are mapped to screen
 * pixels through a calibration rectangle.  The defaults are a reasonable
 * starting point; call setCalibration() to tune, or readRaw() to gather values
 * for your panel.
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

class chipguy_ESP32_32E_4inch_touch {
public:
    chipguy_ESP32_32E_4inch_touch();

    // Add the XPT2046 to the display's SPI bus and configure the IRQ pin.
    // MUST be called after chipguy_ESP32_32E_4inch_display::begin(), which initializes
    // the shared bus.  Returns false if the device could not be added.
    bool begin();

    // Match the touch coordinate mapping to the display rotation, using the
    // same convention as chipguy_ESP32_32E_4inch_display: an index 0..3 or degrees
    // 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Override the raw-ADC calibration rectangle.  xMin/xMax map across the
    // panel's physical X axis (320 px), yMin/yMax across the physical Y axis
    // (480 px), both before rotation is applied.
    void setCalibration(uint16_t xMin, uint16_t xMax, uint16_t yMin, uint16_t yMax);

    // Minimum estimated pressure (z1 + 4095 - z2) to accept as a press.  Higher
    // = requires a firmer touch and rejects more light-contact noise; lower =
    // more sensitive.  Default ~400.
    void setPressureThreshold(uint16_t z);

    // Read the touch point.  Returns true if the screen is currently pressed
    // (firmly enough to clear the pressure threshold), with x,y mapped to
    // screen pixels for the active rotation.
    bool read(uint16_t &x, uint16_t &y);

    // Read median-filtered raw 12-bit ADC values (no calibration/rotation
    // applied).  Returns true if pressed.  Useful for gathering calibration
    // numbers.
    bool readRaw(uint16_t &rawX, uint16_t &rawY);

private:
    spi_device_handle_t _spi = nullptr;
    uint8_t  _rotation = 0;
    uint16_t _xMin, _xMax, _yMin, _yMax;
    uint16_t _zThreshold;

    uint16_t _readChannel(uint8_t cmd);
    uint16_t _readChannelMedian(uint8_t cmd);
    bool     _readRawSample(uint16_t &rawX, uint16_t &rawY);
};
