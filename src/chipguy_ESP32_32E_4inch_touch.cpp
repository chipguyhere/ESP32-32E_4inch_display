/*
 * XPT2046 resistive touch driver for the ESP32-32E 4.0" touch LCD.
 * See chipguy_ESP32_32E_4inch_touch.h for wiring and usage.
 */

#include "chipguy_ESP32_32E_4inch_touch.h"
#include "chipguy_ESP32_32E_4inch_display.h"   // for E32E_TFT_SPI_HOST and NATIVE_WIDTH/HEIGHT

// ── Board pin assignments (fixed on the ESP32-32E board) ──────────────────────────
// SCLK/MOSI/MISO are shared with the display and already owned by the bus the
// display's begin() initialized; only CS and IRQ are touch-specific here.
#define E32E_TOUCH_CS    33
#define E32E_TOUCH_IRQ   36   // PENIRQ, active LOW while the panel is being touched

// XPT2046 control bytes: start bit + channel select, 12-bit, differential.
#define XPT2046_CMD_X   0xD0   // read X position
#define XPT2046_CMD_Y   0x90   // read Y position
#define XPT2046_CMD_Z1  0xB0   // read Z1 (pressure)
#define XPT2046_CMD_Z2  0xC0   // read Z2 (pressure)

// Minimum estimated pressure (z1 + 4095 - z2) to accept as a real press.
// Lower = more sensitive; higher = requires a firmer touch.  Tunable at
// runtime with setPressureThreshold().  Light/lifting contact reads below this
// and produces wild, jumpy X/Y, so we reject it.
#define E32E_TOUCH_Z_THRESHOLD  400

// XPT2046's max SPI clock is ~2.5 MHz; stay comfortably under it.  This device
// shares the display's bus, so the SPI master driver re-clocks the bus to this
// rate for touch transactions and back to 40 MHz for the display.
#define E32E_TOUCH_SPI_HZ  2000000

// Default raw-ADC calibration rectangle (typical panel; tune with readRaw()).
#define E32E_TOUCH_X_MIN  200
#define E32E_TOUCH_X_MAX  3900
#define E32E_TOUCH_Y_MIN  200
#define E32E_TOUCH_Y_MAX  3900

// Samples taken per axis per read.  We return the MEDIAN of these (not the
// mean), which discards the occasional spike instead of blending it in.  Odd
// so the median is a single middle element.
static const int E32E_TOUCH_SAMPLES = 7;

chipguy_ESP32_32E_4inch_touch::chipguy_ESP32_32E_4inch_touch()
    : _xMin(E32E_TOUCH_X_MIN), _xMax(E32E_TOUCH_X_MAX),
      _yMin(E32E_TOUCH_Y_MIN), _yMax(E32E_TOUCH_Y_MAX),
      _zThreshold(E32E_TOUCH_Z_THRESHOLD) {}

bool chipguy_ESP32_32E_4inch_touch::begin() {
    // IRQ is GPIO36, an input-only pin with NO internal pull resistor; the
    // XPT2046's PENIRQ drives it high when idle, low when touched.
    pinMode(E32E_TOUCH_IRQ, INPUT);

    // Add the XPT2046 to the display's already-initialized SPI bus as a second
    // device.  Hardware CS on GPIO33, slow clock, SPI mode 0.
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = E32E_TOUCH_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = E32E_TOUCH_CS;   // hardware-managed CS
    devcfg.queue_size = 1;

    esp_err_t err = spi_bus_add_device(E32E_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("touch: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }
    return true;
}

void chipguy_ESP32_32E_4inch_touch::setRotation(uint16_t rotation) {
    _rotation = e32e_normalize_rotation(rotation);
}

void chipguy_ESP32_32E_4inch_touch::setCalibration(uint16_t xMin, uint16_t xMax,
                                            uint16_t yMin, uint16_t yMax) {
    _xMin = xMin; _xMax = xMax;
    _yMin = yMin; _yMax = yMax;
}

void chipguy_ESP32_32E_4inch_touch::setPressureThreshold(uint16_t z) {
    _zThreshold = z;
}

// One XPT2046 conversion over the shared SPI bus: send the 8-bit command, then
// clock out the 12-bit result.  A 3-byte transaction (command + 2 read bytes)
// covers the command, the one busy clock, and the 12 result bits MSB-first; the
// result lands in bits 14..3 of the last two bytes.
uint16_t chipguy_ESP32_32E_4inch_touch::_readChannel(uint8_t cmd) {
    spi_transaction_t t = {};
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 24;               // 3 bytes on the wire
    t.tx_data[0] = cmd;
    t.tx_data[1] = 0;
    t.tx_data[2] = 0;
    if (spi_device_polling_transmit(_spi, &t) != ESP_OK) return 0;
    return (uint16_t)(((t.rx_data[1] << 8) | t.rx_data[2]) >> 3) & 0x0FFF;
}

// Read one channel E32E_TOUCH_SAMPLES times and return the MEDIAN.  The first
// conversion right after switching channels is the least settled, so we take a
// throwaway read before sampling.  The median rejects outliers (the spikes that
// make a plain average jump around) far better than a mean.
uint16_t chipguy_ESP32_32E_4inch_touch::_readChannelMedian(uint8_t cmd) {
    uint16_t buf[E32E_TOUCH_SAMPLES];
    _readChannel(cmd);                                   // throwaway: let it settle
    for (int i = 0; i < E32E_TOUCH_SAMPLES; i++) buf[i] = _readChannel(cmd);

    // Insertion sort (tiny fixed array), then take the middle element.
    for (int i = 1; i < E32E_TOUCH_SAMPLES; i++) {
        uint16_t v = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = v;
    }
    return buf[E32E_TOUCH_SAMPLES / 2];
}

bool chipguy_ESP32_32E_4inch_touch::_readRawSample(uint16_t &rawX, uint16_t &rawY) {
    if (!_spi) return false;
    // IRQ is pulled low by the controller only while the panel is pressed.
    if (digitalRead(E32E_TOUCH_IRQ) != LOW) return false;

    // Pressure gate.  The XPT2046 measures touch pressure as two ADC readings;
    // z1 + 4095 - z2 rises with how hard the panel is pressed.  A light or
    // lifting touch reads low and returns garbage X/Y, so reject it.  Raise the
    // threshold (setPressureThreshold) to require a firmer press.
    int z1 = _readChannel(XPT2046_CMD_Z1);
    int z2 = _readChannel(XPT2046_CMD_Z2);
    int z = z1 + 4095 - z2;
    if (z < (int)_zThreshold) return false;

    rawX = _readChannelMedian(XPT2046_CMD_X);
    rawY = _readChannelMedian(XPT2046_CMD_Y);

    // Still pressed after sampling?  Rejects spurious single-edge IRQs and
    // readings taken across a release.
    if (digitalRead(E32E_TOUCH_IRQ) != LOW) return false;

    return true;
}

bool chipguy_ESP32_32E_4inch_touch::readRaw(uint16_t &rawX, uint16_t &rawY) {
    return _readRawSample(rawX, rawY);
}

bool chipguy_ESP32_32E_4inch_touch::read(uint16_t &x, uint16_t &y) {
    uint16_t rawX, rawY;
    if (!_readRawSample(rawX, rawY)) return false;

    // Clamp to the calibration rectangle, then map to physical pixels
    // (320 wide x 480 tall, before rotation).
    long px = map((long)constrain(rawX, _xMin, _xMax), _xMin, _xMax,
                  0, E32E_TFT_NATIVE_WIDTH - 1);
    long py = map((long)constrain(rawY, _yMin, _yMax), _yMin, _yMax,
                  0, E32E_TFT_NATIVE_HEIGHT - 1);

    // On this panel the XPT2046's raw axes run opposite the ST7796's, so we
    // mirror in physical space (before rotation, so every rotation inherits the
    // correction).  Measured on hardware in native portrait: X reads correctly
    // un-mirrored, while Y must be flipped.  If a future unit reads mirrored on
    // an axis, flip the corresponding line below.
    py = (E32E_TFT_NATIVE_HEIGHT - 1) - py;

    const long W = E32E_TFT_NATIVE_WIDTH - 1;    // 319
    const long H = E32E_TFT_NATIVE_HEIGHT - 1;   // 479

    switch (_rotation) {
        case 0:  x = px;          y = py;          break;  // portrait
        case 1:  x = py;          y = W - px;      break;  // landscape
        case 2:  x = W - px;      y = H - py;      break;  // portrait flipped
        case 3:  x = H - py;      y = px;          break;  // landscape flipped
    }
    return true;
}
