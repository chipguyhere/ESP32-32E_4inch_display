/*
 * ST7796 SPI display driver for the ESP32-32E 4.0" touch LCD.
 * See chipguy_ESP32_32E_4inch_display.h for the wiring and usage overview.
 */

#include "chipguy_ESP32_32E_4inch_display.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>

// ── Board pin assignments (fixed on the ESP32-32E board) ──────────────────────────
// Display and touch share SCLK/MOSI/MISO; only the chip-selects differ.
#define E32E_TFT_MOSI  13
#define E32E_TFT_MISO  12   // shared with touch (XPT2046 DOUT); needed for reads
#define E32E_TFT_SCLK  14
#define E32E_TFT_CS    15
#define E32E_TFT_DC     2
#define E32E_TFT_BL    27   // backlight, active HIGH
// Display RST is tied to the ESP32 EN (chip reset); there is no GPIO reset, so
// the panel is reset in software (command 0x01) inside _initPanel().

// ST7796 tolerates 40 MHz comfortably over the short board traces.  The
// touch device adds itself to this same bus at a much slower clock (see the
// touch driver); the SPI master driver arbitrates between them.
#define E32E_TFT_SPI_HZ  40000000

// The ST7796 on this board uses BGR color order; this is the base MADCTL with
// only the BGR bit set.  Rotation bits (MY/MX/MV) are OR'd in per rotation.
#define E32E_MADCTL_BGR  0x08
#define ST7796_MADCTL_MY  0x80
#define ST7796_MADCTL_MX  0x40
#define ST7796_MADCTL_MV  0x20

// Largest single SPI transfer we size DMA descriptors for: a full screen, so
// any partial flush region fits in one queued transaction.
static const size_t E32E_MAX_TRANSFER =
    (size_t)E32E_TFT_NATIVE_WIDTH * E32E_TFT_NATIVE_HEIGHT * 2;

// CS is driven by the SPI peripheral (hardware CS), but DC is a plain GPIO.
// The driver calls this just before each transaction goes on the bus — for
// queued transactions that's in ISR context — and we set DC from the
// transaction's `user` field: 0 = command, 1 = data.
static void _e32e_spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)E32E_TFT_DC, (int)(intptr_t)t->user);
}

chipguy_ESP32_32E_4inch_display::chipguy_ESP32_32E_4inch_display() {}

// ── Low-level SPI helpers ──────────────────────────────────────────────────
// DC and CS are handled by the pre-callback and hardware CS respectively, so
// these just submit the bytes.  All command/parameter traffic is synchronous
// (polling); only the pixel blit is queued asynchronously (see flushRegion).

void chipguy_ESP32_32E_4inch_display::_cmd(uint8_t cmd) {
    if (!_spi) return;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)0;          // DC = command
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_ESP32_32E_4inch_display::_data(const uint8_t *data, size_t len) {
    if (!_spi || len == 0) return;
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_ESP32_32E_4inch_display::_data8(uint8_t val) {
    _data(&val, 1);
}

// ── ST7796 power-on init sequence ──────────────────────────────────────────
// Standard ST7796 startup (the values used by TFT_eSPI's ST7796 profile).
// Because RST is tied to the chip's EN, we begin with a SOFTWARE reset (0x01)
// instead of a hardware reset pulse.  MADCTL is written here as a placeholder
// and then re-issued by setRotation().
void chipguy_ESP32_32E_4inch_display::_initPanel() {
    _cmd(0x01);                            // Software reset (no GPIO RST on this board)
    delay(120);
    _cmd(0x11);                            // Sleep out
    delay(120);

    _cmd(0xF0); _data8(0xC3);              // Command Set Control — enable ext command 2 part I
    _cmd(0xF0); _data8(0x96);              // Command Set Control — enable ext command 2 part II

    _cmd(0x36); _data8(E32E_MADCTL_BGR);    // MADCTL (overwritten by setRotation)
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565

    _cmd(0xB4); _data8(0x01);              // Column inversion
    _cmd(0xB6); { const uint8_t d[] = {0x80, 0x02, 0x3B}; _data(d, 3); }   // Display function control
    _cmd(0xE8); { const uint8_t d[] = {0x40, 0x8A, 0x00, 0x00, 0x29,
                  0x19, 0xA5, 0x33}; _data(d, 8); }                        // Display output ctrl adjust
    _cmd(0xC1); _data8(0x06);              // Power control 2
    _cmd(0xC2); _data8(0xA7);              // Power control 3
    _cmd(0xC5); _data8(0x18);              // VCOM control
    delay(120);

    _cmd(0xE0); { const uint8_t d[] = {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F,
                  0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}; _data(d, 14); } // Positive gamma
    _cmd(0xE1); { const uint8_t d[] = {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B,
                  0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}; _data(d, 14); } // Negative gamma
    delay(120);

    _cmd(0xF0); _data8(0x3C);              // Command Set Control — disable ext command 2 part I
    _cmd(0xF0); _data8(0x69);              // Command Set Control — disable ext command 2 part II
    delay(120);

    _cmd(0x29);                            // Display on
    delay(20);
}

void chipguy_ESP32_32E_4inch_display::_setAddrWindow(int x0, int y0, int x1, int y1) {
    uint8_t col[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    _cmd(0x2A);              // CASET — Column Address Set
    _data(col, 4);

    uint8_t row[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    _cmd(0x2B);              // RASET — Row Address Set
    _data(row, 4);

    _cmd(0x2C);              // RAMWR — Memory Write
}

// Synchronous (blocking) pixel push, used by fillScreen().  The interactive
// path uses the queued/async flushRegion() instead.
void chipguy_ESP32_32E_4inch_display::_pushPixels(const uint8_t *data, size_t bytes) {
    if (!_spi || bytes == 0) return;
    spi_transaction_t t = {};
    t.length = bytes * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

// Block until the in-flight async pixel DMA (if any) has completed, so the bus
// is idle before we touch it directly (or before the touch driver uses it).
void chipguy_ESP32_32E_4inch_display::_waitPixelDone() {
    if (_pixelBusy) {
        spi_transaction_t *result = nullptr;
        spi_device_get_trans_result(_spi, &result, portMAX_DELAY);
        _pixelBusy = false;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool chipguy_ESP32_32E_4inch_display::begin(uint16_t rotation) {
    // DC is driven by the pre-transaction callback; configure it as output.
    // CS is owned by the SPI peripheral (hardware CS), so we don't touch it.
    // There is no GPIO reset line on this board (RST ties to EN), so the only
    // reset is the software reset issued in _initPanel().
    gpio_set_direction((gpio_num_t)E32E_TFT_DC, GPIO_MODE_OUTPUT);

    // Register the SPI bus with the ESP-IDF master driver.  MISO IS included
    // here (unlike a write-only display) because the XPT2046 touch controller
    // shares this bus and returns its samples on MISO.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = E32E_TFT_MOSI;
    buscfg.miso_io_num = E32E_TFT_MISO;   // shared with touch — must be wired
    buscfg.sclk_io_num = E32E_TFT_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = E32E_MAX_TRANSFER;

    esp_err_t err = spi_bus_initialize(E32E_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("display: spi_bus_initialize failed: 0x%x\n", err);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = E32E_TFT_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = E32E_TFT_CS;   // hardware-managed CS
    devcfg.queue_size = 1;              // one async pixel transfer in flight
    devcfg.pre_cb = _e32e_spi_pre_cb;    // sets DC from each transaction's user flag
    devcfg.flags = SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(E32E_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("display: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }

    _initPanel();
    setRotation(rotation);

    // Backlight on at full brightness.
    setBacklight(100);

    return true;
}

void chipguy_ESP32_32E_4inch_display::setRotation(uint16_t rotation) {
    _waitPixelDone();           // bus must be idle before we send a command
    _rotation = e32e_normalize_rotation(rotation);
    uint8_t madctl = E32E_MADCTL_BGR;

    switch (_rotation) {
        case 0:  // portrait, native
            madctl |= ST7796_MADCTL_MX;
            _w = E32E_TFT_NATIVE_WIDTH;
            _h = E32E_TFT_NATIVE_HEIGHT;
            break;
        case 1:  // landscape
            madctl |= ST7796_MADCTL_MV;
            _w = E32E_TFT_NATIVE_HEIGHT;
            _h = E32E_TFT_NATIVE_WIDTH;
            break;
        case 2:  // portrait flipped
            madctl |= ST7796_MADCTL_MY;
            _w = E32E_TFT_NATIVE_WIDTH;
            _h = E32E_TFT_NATIVE_HEIGHT;
            break;
        case 3:  // landscape flipped
            madctl |= ST7796_MADCTL_MV | ST7796_MADCTL_MX | ST7796_MADCTL_MY;
            _w = E32E_TFT_NATIVE_HEIGHT;
            _h = E32E_TFT_NATIVE_WIDTH;
            break;
    }

    _cmd(0x36);          // MADCTL
    _data8(madctl);
}

void chipguy_ESP32_32E_4inch_display::setBacklight(int percentage) {
    if (percentage < 0)   percentage = 0;
    if (percentage > 100) percentage = 100;
    // Active HIGH: higher duty = brighter.
    analogWrite(E32E_TFT_BL, percentage * 255 / 100);
}

// Asynchronous flush.  Returns once this region's pixel DMA has been *queued*,
// not when it finishes — the caller (LVGL via lv_setup) is then free to render
// the next region into the other buffer while this one transmits.  The bus is
// serialized by blocking on the *previous* flush's DMA at the top of this one.
void chipguy_ESP32_32E_4inch_display::flushRegion(int x1, int y1, int x2, int y2,
                                           uint16_t *pixels) {
    if (!_spi) return;

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    size_t count = (size_t)w * h;

    // The swap below mutates THIS region's buffer.  With double buffering LVGL
    // hands us a different buffer than the one the previous flush is still
    // DMA'ing, so the swap safely overlaps that transfer.  Guard the (not
    // expected) case where we're handed the in-flight buffer: wait first.
    if (_pixelBusy && (const void *)pixels == _pixelTrans.tx_buffer) {
        _waitPixelDone();
    }

    // LVGL renders RGB565 little-endian (native ESP32); the ST7796 expects
    // big-endian (MSB first).  Swap in place — overlaps the previous region's
    // in-flight DMA (different buffer) for free.
    for (size_t i = 0; i < count; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

    // Now wait for the previous flush's DMA to finish, so the SPI bus is free
    // for this region's address window and pixel transfer.
    _waitPixelDone();

    // Address window (synchronous commands), then queue the pixel blit.
    _setAddrWindow(x1, y1, x2, y2);

    _pixelTrans = {};
    _pixelTrans.length = count * 16;        // bits (2 bytes per pixel)
    _pixelTrans.tx_buffer = pixels;
    _pixelTrans.user = (void *)1;           // DC = data
    if (spi_device_queue_trans(_spi, &_pixelTrans, portMAX_DELAY) == ESP_OK) {
        _pixelBusy = true;
    }
}

void chipguy_ESP32_32E_4inch_display::waitFlush() {
    _waitPixelDone();
}

void chipguy_ESP32_32E_4inch_display::fillScreen(uint16_t color) {
    if (!_spi) return;
    _waitPixelDone();           // drain any queued async flush first
    uint16_t be = __builtin_bswap16(color);

    // One-row buffer pushed for every row.  Must be DMA-capable: the ESP-IDF
    // SPI driver requires DMA memory for transfers larger than the FIFO.
    uint16_t *line = (uint16_t *)heap_caps_malloc((size_t)_w * 2, MALLOC_CAP_DMA);
    if (!line) return;
    for (int i = 0; i < _w; i++) line[i] = be;

    _setAddrWindow(0, 0, _w - 1, _h - 1);
    for (int y = 0; y < _h; y++) {
        _pushPixels((const uint8_t *)line, (size_t)_w * 2);
    }
    heap_caps_free(line);
}
