#pragma once
#include <Arduino.h>

// ===== WiFi =====
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"

// ===== タッチ (XPT2046) : CYD は別SPI(VSPI)に接続 =====
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// ===== 音声出力 =====
// CYDのスピーカー端子(アンプ入力)は GPIO26 = ESP32内蔵DAC2 に接続されている
// (GPIO25 はタッチのCLKと競合するため LEFT のみ使用)

#define DEFAULT_VOLUME 10   // 0..21

// 画面の上下が逆に見える場合は 1 にする (180度回転)
#define SCREEN_FLIP 0

struct Station {
  const char* name;
  const char* url;   // MP3/AAC ストリーム (http 推奨。https も動くが TLS でメモリを使う)
};

// 好きな局に書き換えてください (局名は日本語もOK / UTF-8)
const Station STATIONS[] = {
  {"SomaFM Groove Salad",    "http://ice1.somafm.com/groovesalad-128-mp3"},
  {"SomaFM Drone Zone",      "http://ice1.somafm.com/dronezone-128-mp3"},
  {"SomaFM Secret Agent",    "http://ice1.somafm.com/secretagent-128-mp3"},
  {"Radio Paradise",         "http://stream.radioparadise.com/mp3-128"},
  {"LISTEN.moe J-POP",       "https://listen.moe/stream"},
  {"五反野 FM",              "https://radio.gotanno.love/"},
  {"ジャズ Sakura",          "http://kathy.torontocast.com:3330/stream/1/?esPlayer&cb=82181.mp3"},
  {"J-Club Club Bandstand",  "https://cast1.torontocast.com:2060/;.mp3"},
  {"Japan Hits - asia DREAM","http://quincy.torontocast.com:2020/stream.mp3"},
};
const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
