#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// CYD (ESP32-2432S028R) 表示部のみ。タッチは XPT2046_Touchscreen ライブラリで処理する。

// パネルの種類 (表示がおかしい場合はここを切り替える)
//   2 : ILI9342 系 (横長 320x240 が標準)  ← 既定
//   1 : ILI9341 系 (縦長 240x320 が標準)
#define CYD_PANEL 2

#if CYD_PANEL == 2
  using CydPanel = lgfx::Panel_ILI9342;
  #define CYD_PANEL_W      320
  #define CYD_PANEL_H      240
  #define CYD_OFFSET_ROT   0
  #define CYD_LANDSCAPE    0     // 横向きになる rotation 値 (反対向きは +2)
#else
  using CydPanel = lgfx::Panel_ILI9341;
  #define CYD_PANEL_W      240
  #define CYD_PANEL_H      320
  #define CYD_OFFSET_ROT   1
  #define CYD_LANDSCAPE    1
#endif

class LGFX : public lgfx::LGFX_Device {
  CydPanel            _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = HSPI_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs   = 15;
      cfg.pin_rst  = -1;
      cfg.pin_busy = -1;
      cfg.panel_width   = CYD_PANEL_W;
      cfg.panel_height  = CYD_PANEL_H;
      cfg.memory_width  = CYD_PANEL_W;
      cfg.memory_height = CYD_PANEL_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = CYD_OFFSET_ROT;
      cfg.readable  = false;
      cfg.invert    = false;   // (この基板では false が正解)
      cfg.rgb_order = true;    // (この基板では true が正解)
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
