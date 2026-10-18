// Minimal UC8179 driver for the reTerminal E1001's GDEY075T7 panel, built on GxEPD2's SPI/busy
// plumbing. The init sequence is GxEPD2_750_GDEY075T7's (Jean-Marc Zingg, GPL-3.0).
//
// We keep our own 800x480 framebuffer (bit=1 is WHITE, MSB first) and always write both
// controller buffers (0x10 old, 0x13 new): on the E1001 the panel's power is switched by RST,
// so its RAM is lost in deep sleep and we restore "old" from flash for partial refreshes.
#pragma once
#include <GxEPD2_EPD.h>

enum RefreshMode { REFRESH_AUTO, REFRESH_FULL, REFRESH_FAST, REFRESH_PARTIAL };

class Panel : public GxEPD2_EPD {
public:
  static const uint16_t WIDTH = 800, HEIGHT = 480;
  static const uint32_t BYTES = WIDTH / 8 * HEIGHT;

  Panel(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_EPD(cs, dc, rst, busy, LOW, 10000000, WIDTH, HEIGHT, GxEPD2::GDEY075T7, false, true, true) {}

  // old may be nullptr when we don't know what is on the glass.
  void show(const uint8_t* img, const uint8_t* old, RefreshMode mode) {
    bool partial = mode == REFRESH_PARTIAL && old;
    _initDisplay(partial);
    _writeBuffer(0x10, old, 0x00);
    _writeBuffer(0x13, img, 0xFF);
    if (partial) {
      _writeCommand(0x90);  // partial window = whole screen
      const uint8_t win[] = {0, 0, (WIDTH - 1) >> 8, (WIDTH - 1) & 0xFF, 0, 0, (HEIGHT - 1) >> 8, (HEIGHT - 1) & 0xFF, 0x01};
      _writeData(win, sizeof(win));
    } else if (mode == REFRESH_FAST) {
      _writeCommand(0xE0); _writeData(0x02);  // TSFIX
      _writeCommand(0xE5); _writeData(0x5A);  // pretend 90C -> short waveform
    } else {
      // real OTP waveform at the measured temperature: slowest, but deepest black / cleanest white
      _writeCommand(0xE0); _writeData(0x00);
      _writeCommand(0x41); _writeData(0x00);
    }
    _writeCommand(0x12);
    _waitWhileBusy("refresh", partial ? 500 : 4000);
    powerOff();
  }

  void powerOff() override {
    if (_power_is_on) {
      _writeCommand(0x02);
      _waitWhileBusy("powerOff", 250);
    }
    _power_is_on = false;
  }

  void hibernate() override {
    powerOff();
    _writeCommand(0x07);  // deep sleep
    _writeData(0xA5);
    _hibernating = true;
  }

  // unused GxEPD2_EPD interface
  void clearScreen(uint8_t) override {}
  void writeScreenBuffer(uint8_t) override {}
  void writeImage(const uint8_t*, int16_t, int16_t, int16_t, int16_t, bool, bool, bool) override {}
  void writeImagePart(const uint8_t*, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, bool, bool, bool) override {}
  void refresh(bool) override {}
  void refresh(int16_t, int16_t, int16_t, int16_t) override {}

private:
  void _initDisplay(bool partial) {
    if (_hibernating) { _reset(); _hibernating = false; }
    _writeCommand(0x00); _writeData(0x1f);  // panel setting: KW, LUT from OTP
    const uint8_t pwr[] = {0x07, 0x07, 0x3f, 0x3f, 0x09};
    _writeCommand(0x01); _writeData(pwr, sizeof(pwr));
    const uint8_t boost[] = {0x17, 0x17, 0x28, 0x17};
    _writeCommand(0x06); _writeData(boost, sizeof(boost));
    const uint8_t res[] = {WIDTH >> 8, WIDTH & 0xFF, HEIGHT >> 8, HEIGHT & 0xFF};
    _writeCommand(0x61); _writeData(res, sizeof(res));
    _writeCommand(0x15); _writeData(0x00);
    _writeCommand(0x50); _writeData(0x29); _writeData(0x07);
    _writeCommand(0x60); _writeData(0x22);
    _writeCommand(0xE3); _writeData(0x22);
    if (partial) {  // fast partial waveform from OTP
      _writeCommand(0xE0); _writeData(0x02);
      _writeCommand(0xE5); _writeData(0x6E);
    }
    if (!_power_is_on) {
      _writeCommand(0x04);
      _waitWhileBusy("powerOn", 200);
      _power_is_on = true;
    }
  }

  void _writeBuffer(uint8_t cmd, const uint8_t* buf, uint8_t fill) {
    _writeCommand(cmd);
    _startTransfer();
    for (uint32_t i = 0; i < BYTES; i++) _transfer(buf ? buf[i] : fill);
    _endTransfer();
  }
};
