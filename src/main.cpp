#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include "LGFX_CYD.h"
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "Audio.h"
#include "config.h"

// ---------------- デバイス ----------------
LGFX tft;

// 日本語フォント (LovyanGFX 内蔵)
#define F16 (&fonts::lgfxJapanGothicP_16)
#define F24 (&fonts::lgfxJapanGothicP_24)
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Audio audio(true, I2S_DAC_CHANNEL_LEFT_EN);   // 内蔵DAC → GPIO26
Preferences prefs;

// ---------------- 状態 (UIタスク ⇔ 音声タスク) ----------------
volatile int  reqStation = -1;     // >=0 : その局に接続
volatile int  reqVolume  = -1;     // >=0 : 音量変更
volatile bool reqStop    = false;

int  curStation = 0;
int  volume     = DEFAULT_VOLUME;
volatile bool playing = false;

char streamTitle[96] = "";
volatile bool titleDirty = true;
volatile uint32_t lastStartMs = 0;
volatile bool reconnectReq = false;   // 接続エラー/切断時に、局の元URLから接続し直す要求
volatile uint16_t peakL = 0, peakR = 0;   // audio_process_i2s() が更新 / UI側が読んでクリア

// ---------------- 画面レイアウト ----------------
const int BTN_Y = 178, BTN_W = 58, BTN_H = 54, BTN_GAP = 5;
enum { B_PREV, B_PLAY, B_NEXT, B_VDN, B_VUP, B_COUNT };

int btnX(int i) { return 5 + i * (BTN_W + BTN_GAP); }

void drawButton(int i) {
  int x = btnX(i), cx = x + BTN_W / 2, cy = BTN_Y + BTN_H / 2;
  tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 8, TFT_NAVY);
  tft.drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 8, TFT_LIGHTGREY);
  switch (i) {
    case B_PREV:
      tft.fillTriangle(cx + 10, cy - 12, cx + 10, cy + 12, cx - 4, cy, TFT_WHITE);
      tft.fillRect(cx - 12, cy - 12, 4, 24, TFT_WHITE);
      break;
    case B_NEXT:
      tft.fillTriangle(cx - 10, cy - 12, cx - 10, cy + 12, cx + 4, cy, TFT_WHITE);
      tft.fillRect(cx + 8, cy - 12, 4, 24, TFT_WHITE);
      break;
    case B_PLAY:
      if (playing) tft.fillRect(cx - 10, cy - 11, 20, 22, TFT_WHITE);                    // 停止
      else tft.fillTriangle(cx - 8, cy - 13, cx - 8, cy + 13, cx + 12, cy, TFT_WHITE);   // 再生
      break;
    case B_VDN:
      tft.fillRect(cx - 10, cy - 2, 20, 4, TFT_WHITE);
      break;
    case B_VUP:
      tft.fillRect(cx - 10, cy - 2, 20, 4, TFT_WHITE);
      tft.fillRect(cx - 2, cy - 10, 4, 20, TFT_WHITE);
      break;
  }
}

void drawHeader() {
  tft.fillRect(0, 0, 320, 26, TFT_DARKGREY);
  tft.setFont(F16);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("CYD Web Radio", 8, 5);
  tft.setTextColor(playing ? TFT_GREEN : TFT_ORANGE, TFT_DARKGREY);
  tft.drawRightString(playing ? "PLAY" : "STOP", 312, 5);
}

void drawStation() {
  tft.fillRect(0, 30, 320, 56, TFT_BLACK);
  // 長い局名は小さいフォントに切り替える
  tft.setFont(F24);
  const lgfx::IFont* f = F24;
  if (tft.textWidth(STATIONS[curStation].name) > 304) { f = F16; tft.setFont(F16); }
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(STATIONS[curStation].name, 8, (f == F24) ? 32 : 36);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d/%d", curStation + 1, STATION_COUNT);
  tft.setFont(F16);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawRightString(buf, 312, 64);
}

// UTF-8文字列を maxW ピクセル幅で lines 行に折り返す (現在のフォントで幅を測定)
void wrapUtf8(const char* src, char out[][96], int lines, int maxW) {
  for (int i = 0; i < lines; i++) out[i][0] = 0;
  int li = 0, len = 0;
  const uint8_t* p = (const uint8_t*)src;
  while (*p && li < lines) {
    int n = (*p < 0x80) ? 1 : ((*p >> 5) == 0x6) ? 2 : ((*p >> 4) == 0xE) ? 3 : ((*p >> 3) == 0x1E) ? 4 : 1;
    char ch[5] = {0};
    int m = 0;
    while (m < n && p[m]) { ch[m] = (char)p[m]; m++; }      // 途中で切れた文字列への備え
    if (len + m >= 95) { li++; len = 0; if (li >= lines) break; out[li][0] = 0; }
    memcpy(out[li] + len, ch, m);
    out[li][len + m] = 0;
    if (len > 0 && tft.textWidth(out[li]) > maxW) {          // はみ出したら次の行へ送る
      out[li][len] = 0;
      li++; len = 0;
      if (li >= lines) break;
      memcpy(out[li], ch, m);
      out[li][m] = 0;
    }
    len += m;
    p += m;
  }
}

void drawTitle() {
  tft.fillRect(0, 88, 320, 38, TFT_BLACK);
  tft.setFont(F16);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  char line[2][96];
  wrapUtf8(streamTitle, line, 2, 304);
  for (int i = 0; i < 2; i++) tft.drawString(line[i], 8, 89 + i * 18);
}

void drawVolume() {
  tft.fillRect(0, 148, 320, 24, TFT_BLACK);
  tft.setFont(F16);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("VOL", 8, 152);
  const int w = 220, x = 50, y = 153, h = 14;
  tft.drawRect(x, y, w, h, TFT_LIGHTGREY);
  tft.fillRect(x + 2, y + 2, (w - 4) * volume / 21, h - 4, TFT_GREEN);
  char buf[8]; snprintf(buf, sizeof(buf), "%d", volume);
  tft.drawRightString(buf, 312, 152);
}

void drawStatusMsg(const char* msg, uint16_t color = TFT_WHITE) {
  tft.fillRect(0, 88, 320, 38, TFT_BLACK);
  tft.setFont(F16);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, 8, 100);
}

// ---------------- VUメーター ----------------
const int VU_SEGS = 24, VU_X = 24, VU_PITCH = 12, VU_SEG_W = 10, VU_SEG_H = 7;
const int VU_Y[2] = {129, 138};
const uint16_t VU_OFF = 0x2104;   // 消灯セグメントの暗灰色
struct VuCh { int level = 0; int peak = 0; uint32_t peakT = 0; uint16_t cache[VU_SEGS]; };
VuCh vu[2];
float vuRef = 40.0f;              // 自動ゲイン用の基準値

uint16_t vuColor(int i) { return i < 16 ? TFT_GREEN : (i < 21 ? TFT_YELLOW : TFT_RED); }

void vuReset() {
  tft.fillRect(0, 127, 320, 20, TFT_BLACK);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("L", 10, VU_Y[0] - 1);
  tft.drawString("R", 10, VU_Y[1] - 1);
  for (int c = 0; c < 2; c++) {
    vu[c].level = 0; vu[c].peak = 0; vu[c].peakT = 0;
    for (int i = 0; i < VU_SEGS; i++) vu[c].cache[i] = 0;   // 0 = 未描画
  }
}

void vuUpdate() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 33) return;      // 約30fps
  last = now;

  uint16_t pl = peakL, pr = peakR;          // 直近33msのピーク振幅 (0..32768)
  peakL = 0; peakR = 0;
  uint8_t raw[2] = { playing ? (uint8_t)(pl >> 7) : (uint8_t)0,
                     playing ? (uint8_t)(pr >> 7) : (uint8_t)0 };
  float m = max(raw[0], raw[1]);
  vuRef = max(max(vuRef * 0.998f, m), 24.0f);   // 大きい音に合わせ、ゆっくり戻す

  for (int c = 0; c < 2; c++) {
    VuCh& v = vu[c];
    int target = constrain((int)(raw[c] * VU_SEGS / vuRef + 0.5f), 0, VU_SEGS);
    if (target >= v.level) v.level = target;          // 立ち上がりは即時
    else v.level = max(target, v.level - 1);         // 立ち下がりはゆっくり
    if (v.level >= v.peak) { v.peak = v.level; v.peakT = now; }
    else if (now - v.peakT > 600 && v.peak > 0) v.peak--;   // ピークホールド後に落とす

    for (int i = 0; i < VU_SEGS; i++) {
      uint16_t col = (i < v.level) ? vuColor(i) : VU_OFF;
      if (i == v.peak - 1 && v.peak > v.level) col = TFT_WHITE;
      if (v.cache[i] != col) {                        // 変化したセグメントだけ描画
        v.cache[i] = col;
        tft.fillRect(VU_X + i * VU_PITCH, VU_Y[c], VU_SEG_W, VU_SEG_H, col);
      }
    }
  }
}

void drawAll() {
  tft.fillScreen(TFT_BLACK);
  drawHeader(); drawStation(); drawTitle(); drawVolume(); vuReset();
  for (int i = 0; i < B_COUNT; i++) drawButton(i);
}

// ---------------- 再生制御 (UIタスク側から呼ぶ) ----------------
void startStation(int idx) {
  curStation = (idx + STATION_COUNT) % STATION_COUNT;
  streamTitle[0] = 0;
  playing = true;
  reqStation = curStation;
  prefs.putInt("station", curStation);
  drawHeader(); drawStation(); drawTitle(); drawButton(B_PLAY);
}

void stopPlayback() {
  playing = false;
  reqStop = true;
  streamTitle[0] = 0;
  drawHeader(); drawTitle(); drawButton(B_PLAY);
}

void setVol(int v) {
  volume = constrain(v, 0, 21);
  reqVolume = volume;
  prefs.putInt("volume", volume);
  drawVolume();
}

// ---------------- 音声タスク (Core0) ----------------
void audioTask(void*) {
  uint32_t retryDelayMs = 2000;   // 再接続の待ち時間 (失敗が続くと 15 秒まで延ばす)
  uint32_t reconnectAt  = 0;      // 0 以外: この時刻に再接続する

  for (;;) {
    if (reqVolume >= 0) { audio.setVolume(reqVolume); reqVolume = -1; }
    if (reqStop)        { audio.stopSong(); reqStop = false; reconnectAt = 0; reconnectReq = false; }
    if (reqStation >= 0) {
      int idx = reqStation; reqStation = -1;
      audio.stopSong();
      audio.connecttohost(STATIONS[idx].url);
      lastStartMs = millis();
      retryDelayMs = 2000; reconnectAt = 0; reconnectReq = false;
    }

    audio.loop();

    // 長く再生できていれば、待ち時間を元に戻す
    if (playing && audio.isRunning() && millis() - lastStartMs > 20000) retryDelayMs = 2000;

    // 再生中なのに止まってしまった場合も、再接続を要求する
    if (playing && !audio.isRunning() && !reconnectAt && millis() - lastStartMs > 15000) reconnectReq = true;

    // 再接続の予約 (少し待ってから、必ず局の元URLから接続し直す)
    // ※ ライブラリ任せの再接続は、期限付きトークン付きの転送先URLを使い回して 402 になる
    if (reconnectReq) {
      reconnectReq = false;
      if (playing && !reconnectAt) {
        audio.stopSong();
        reconnectAt = millis() + retryDelayMs;
        if (reconnectAt == 0) reconnectAt = 1;
        retryDelayMs = min<uint32_t>(retryDelayMs * 2, 15000);
      }
    }
    if (reconnectAt && (int32_t)(millis() - reconnectAt) >= 0) {
      reconnectAt = 0;
      if (playing) {
        audio.connecttohost(STATIONS[curStation].url);
        lastStartMs = millis();
      }
    }
    vTaskDelay(1);
  }
}

// ---------------- Audioライブラリ コールバック ----------------
void audio_info(const char* info) {
  Serial.printf("info: %s\n", info);
  if (strstr(info, "Stream lost")) reconnectReq = true;   // ライブラリ内部の再接続より先に、こちらで接続し直す
}
void audio_showstreamtitle(const char* s) {
  if (strncmp(s, "HTTP/", 5) == 0) {   // サーバーのエラー応答 (例: 402) が来た
    snprintf(streamTitle, sizeof(streamTitle), "再接続中: %s", s);
    reconnectReq = true;
  } else {
    strlcpy(streamTitle, s, sizeof(streamTitle));
  }
  titleDirty = true;
}
// I2S出力直前に呼ばれるフック。サンプルは上位16bit=L, 下位16bit=R (音量適用後)
// ※ *continueI2S = true にしないと音が出なくなる
void audio_process_i2s(uint32_t* sample, bool* continueI2S) {
  int16_t l = (int16_t)(*sample >> 16);
  int16_t r = (int16_t)(*sample & 0xFFFF);
  uint16_t al = (l < 0) ? -l : l;
  uint16_t ar = (r < 0) ? -r : r;
  if (al > peakL) peakL = al;
  if (ar > peakR) peakR = ar;
  *continueI2S = true;
}
void audio_eof_stream(const char* s)      { Serial.printf("eof: %s\n", s); lastStartMs = 0; }

// ---------------- タッチ ----------------
int hitButton(int x, int y) {
  if (y < BTN_Y || y > BTN_Y + BTN_H) return -1;
  for (int i = 0; i < B_COUNT; i++)
    if (x >= btnX(i) && x <= btnX(i) + BTN_W) return i;
  return -1;
}

void handleTouch() {
  static uint32_t last = 0;
  if (!ts.tirqTouched() || !ts.touched()) return;
  if (millis() - last < 220) return;
  TS_Point p = ts.getPoint();
  int x = constrain(map(p.x, 200, 3700, 0, 319), 0, 319);
  int y = constrain(map(p.y, 240, 3800, 0, 239), 0, 239);
  int b = hitButton(x, y);
  if (b < 0) return;
  last = millis();
  switch (b) {
    case B_PREV: startStation(curStation - 1); break;
    case B_NEXT: startStation(curStation + 1); break;
    case B_PLAY: if (playing) stopPlayback(); else startStation(curStation); break;
    case B_VDN:  setVol(volume - 1); break;
    case B_VUP:  setVol(volume + 1); break;
  }
}

// ---------------- setup / loop ----------------
void connectWiFi() {
  drawStatusMsg("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) {
      drawStatusMsg("WiFi failed. Restarting...", TFT_RED);
      delay(2000);
      ESP.restart();
    }
    delay(250);
  }
  Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation((CYD_LANDSCAPE + (SCREEN_FLIP ? 2 : 0)) & 3);   // 横向き 320x240
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("radio", false);
  curStation = constrain(prefs.getInt("station", 0), 0, STATION_COUNT - 1);
  volume     = constrain(prefs.getInt("volume", DEFAULT_VOLUME), 0, 21);

  connectWiFi();
  audio.setVolume(volume);

  drawAll();
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  startStation(curStation);
}

void loop() {
  handleTouch();
  if (titleDirty) { titleDirty = false; drawTitle(); }
  vuUpdate();
  delay(5);
}
