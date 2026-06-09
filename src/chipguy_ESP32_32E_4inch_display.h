/*
 * ST7796 SPI display driver for the ESP32-32E 4.0" touch LCD
 *
 * This is a generic ESP32 display board sold by several vendors; the PCB
 * silkscreen reads: 4.0" LCD Display, ESP32-32E 320x480, Resistance Touch.
 *
 * Drives the 320x480 ST7796 panel through the ESP-IDF SPI master driver
 * (every transaction goes through spi_device_polling_transmit / queued DMA, so
 * the bus is shared cleanly with the XPT2046 touch controller, which lives on
 * the SAME SPI bus on this board).  Designed to feed an LVGL display in PARTIAL
 * render mode: LVGL hands us a rendered region and we push it with flushRegion().
 *
 * Board wiring (fixed on the ESP32-32E board) — display and touch share one SPI bus:
 *   SCLK 14   MOSI 13   MISO 12   (shared with touch)
 *   Display CS 15   DC 2   BL 27 (backlight, active HIGH)
 *   Display RST is wired to the ESP32 EN (chip reset), so there is no GPIO
 *   reset line; the panel is reset in software (command 0x01) at init.
 *
 * Touch lives in the companion chipguy_ESP32_32E_4inch_touch driver (XPT2046), which
 * shares this driver's SPI bus as a second device with its own chip-select.
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

// SPI host the display (and the companion touch driver) run on.  The display's
// begin() owns spi_bus_initialize(); the touch driver adds itself as a second
// device on this same host.
#define E32E_TFT_SPI_HOST  SPI2_HOST   // HSPI

// Native panel geometry (portrait).  width()/height() reflect the active
// rotation; these are the unrotated physical dimensions.
#define E32E_TFT_NATIVE_WIDTH   320
#define E32E_TFT_NATIVE_HEIGHT  480

// Normalize a rotation argument to an index 0..3.  Accepts either the index
// directly (0..3) or degrees (0/90/180/270) as aliases for 0/1/2/3.
static inline uint8_t e32e_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

class chipguy_ESP32_32E_4inch_display {
public:
    chipguy_ESP32_32E_4inch_display();

    // Initialize the SPI bus, the ST7796 panel, and the backlight.
    // rotation: 0 = portrait (320x480, native), 1 = landscape (480x320),
    //           2 = portrait flipped, 3 = landscape flipped.  Degrees
    //           (0/90/180/270) are accepted as aliases for 0/1/2/3.
    // Returns false if the SPI bus/device could not be set up.
    //
    // NOTE: the bus is initialized with MISO connected (GPIO12) because the
    // XPT2046 touch controller shares it and needs MISO to return data.  Call
    // this before the companion touch driver's begin().
    bool begin(uint16_t rotation = 0);

    // Change rotation after begin().  Updates width()/height() and the
    // panel's MADCTL.  Accepts an index 0..3 or degrees 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Logical dimensions for the current rotation.
    int16_t width()  const { return _w; }
    int16_t height() const { return _h; }

    // Current rotation (0..3).
    uint8_t rotation() const { return _rotation; }

    // Backlight brightness, 0..100 (active HIGH on this board).
    void setBacklight(int percentage);

    // Push a rectangular region of RGB565 pixels to the panel.  Coordinates
    // are inclusive.  `pixels` is in LVGL's native little-endian byte order;
    // this call byte-swaps it in place to the big-endian order the ST7796
    // expects, so the buffer is modified.
    //
    // Asynchronous: it byte-swaps, waits for the PREVIOUS flush's DMA to finish,
    // sends the address window, then QUEUES this region's pixel DMA and returns
    // before it completes.  With double buffering, the caller can render the
    // next region into the other buffer while this one transmits.  `pixels`
    // must stay valid and untouched until the next flushRegion()/waitFlush().
    void flushRegion(int x1, int y1, int x2, int y2, uint16_t *pixels);

    // Block until the most recently queued flush has finished transmitting.
    // Call before tearing down, doing direct drawing after a flush, or sharing
    // the bus with the touch controller.
    void waitFlush();

    // Fill the whole screen with one RGB565 color (handy for bring-up tests).
    void fillScreen(uint16_t color);

private:
    spi_device_handle_t _spi = nullptr;
    int16_t _w = E32E_TFT_NATIVE_WIDTH;
    int16_t _h = E32E_TFT_NATIVE_HEIGHT;
    uint8_t _rotation = 0;

    spi_transaction_t _pixelTrans = {};   // persistent: queued DMA reads it async
    bool _pixelBusy = false;              // true while a queued flush is in flight

    void _cmd(uint8_t cmd);
    void _data(const uint8_t *data, size_t len);
    void _data8(uint8_t val);
    void _initPanel();
    void _setAddrWindow(int x0, int y0, int x1, int y1);
    void _pushPixels(const uint8_t *data, size_t bytes);
    void _waitPixelDone();
};
