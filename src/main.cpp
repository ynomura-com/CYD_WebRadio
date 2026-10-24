#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include "LGFX_CYD.h"
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "Audio.h"
#include "config.h"
#include <esp_log.h>

#if SERIAL_LOG
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGF(...) do {} while (0)
#endif

// patch_audio.py がライブラリ側 (Audio.cpp) に追加する変数。
// weak 定義にしてあるので、パッチが未適用でもビルドは通る (その場合プリバッファは効かない)
__attribute__((weak)) volatile uint32_t g_audioPrebuffer = 0;

// ---------------- デバイス ----------------
LGFX tft;

// 日本語フォント (LovyanGFX 内蔵)
#define F16 (&fonts::lgfxJapanGothicP_16)
#define F24 (&fonts::lgfxJapanGothicP_24)
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
#if AUDIO_OUT_I2S
Audio audio(false);                            // 外付けI2S DAC/アンプ
#else
Audio audio(true, I2S_DAC_CHANNEL_LEFT_EN);    // 内蔵DAC → GPIO26
#endif
Preferences prefs;
bool prefsDirty = false;       // 局/音量の保存待ち (フラッシュ書き込みは音が途切れるので操作が落ち着いてから)
uint32_t prefsDirtyAt = 0;

// ---------------- 状態 (UIタスク ⇔ 音声タスク) ----------------
volatile int  reqStation = -1;     // >=0 : その局に接続
volatile bool reqStop    = false;

int  curStation = 0;
int  volume     = DEFAULT_VOLUME;
volatile bool playing = false;

char streamTitle[96] = "";
volatile bool titleDirty = true;
volatile uint32_t lastStartMs = 0;
volatile bool reconnectReq = false;   // 接続エラー/切断時に、局の元URLから接続し直す要求
volatile int32_t gainQ15 = 0;               // 音量ゲイン (32768 = 等倍)

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
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d", curStation + 1, STATION_COUNT);
  tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
  tft.drawRightString(cnt, 250, 5);
  tft.setTextColor(playing ? TFT_GREEN : TFT_ORANGE, TFT_DARKGREY);
  tft.drawRightString(playing ? "PLAY" : "STOP", 312, 5);
}

void drawStation() {
  tft.fillRect(0, 30, 320, 30, TFT_BLACK);
  // 長い局名は小さいフォントに切り替える
  tft.setFont(F24);
  const lgfx::IFont* f = F24;
  if (tft.textWidth(STATIONS[curStation].name) > 304) { f = F16; tft.setFont(F16); }
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(STATIONS[curStation].name, 8, (f == F24) ? 32 : 36);
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
  tft.fillRect(0, 60, 320, 38, TFT_BLACK);
  tft.setFont(F16);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  char line[2][96];
  wrapUtf8(streamTitle, line, 2, 304);
  for (int i = 0; i < 2; i++) tft.drawString(line[i], 8, 62 + i * 18);
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
  tft.fillRect(0, 60, 320, 38, TFT_BLACK);
  tft.setFont(F16);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(msg, 8, 72);
}

// ---------------- スペクトラム表示 ----------------
// 音声フック (audio_process_i2s) で集めたモノラル音声 (音量を掛ける前) を FFT し、
// 80Hz〜16kHz を対数の間隔で SPEC_BARS 本のバーに分けて表示する。
const int FFT_N = 1024;                          // 2のべき乗 (周波数の分解能 = サンプルレート / FFT_N)
volatile int16_t  specBuf[FFT_N];                // 音声のリングバッファ (書き込み: 音声タスク)
volatile uint32_t specHead = 0;                  // これまでに書き込んだサンプル数

const int   SPEC_BARS = 20, SPEC_X = 10, SPEC_PITCH = 15, SPEC_BAR_W = 12;
const int   SPEC_Y = 100, SPEC_H = 46;           // 表示領域 y=100..145
const float SPEC_FMIN = 80.0f, SPEC_FMAX = 16000.0f;
const float SPEC_RANGE_DB = 45.0f;               // 表示するダイナミックレンジ
const float SPEC_TILT_DB_OCT = 3.0f;             // 高域の持ち上げ (dB/オクターブ)。音楽は高域ほど弱いので見やすくする

static float fftRe[FFT_N], fftIm[FFT_N], fftWin[FFT_N], fftCos[FFT_N / 2], fftSin[FFT_N / 2];
static float fftMag[FFT_N / 2 + 1];
static float specLevel[SPEC_BARS], specPeak[SPEC_BARS];
static uint8_t specPeakHold[SPEC_BARS];
static float specDbMax = -40.0f;                 // 自動ゲイン用の基準 (大きい音に合わせ、ゆっくり戻す)

void specInit() {
  for (int i = 0; i < FFT_N; i++) fftWin[i] = 0.5f - 0.5f * cosf(2.0f * PI * i / (FFT_N - 1));   // ハン窓
  for (int i = 0; i < FFT_N / 2; i++) {
    fftCos[i] = cosf(2.0f * PI * i / FFT_N);
    fftSin[i] = -sinf(2.0f * PI * i / FFT_N);
  }
}

// 基数2の FFT (その場計算)
void fftRun() {
  const int N = FFT_N;
  for (int i = 0, j = 0; i < N - 1; i++) {         // ビット反転
    if (i < j) { float t = fftRe[i]; fftRe[i] = fftRe[j]; fftRe[j] = t; t = fftIm[i]; fftIm[i] = fftIm[j]; fftIm[j] = t; }
    int k = N >> 1;
    while (k <= j) { j -= k; k >>= 1; }
    j += k;
  }
  for (int len = 2; len <= N; len <<= 1) {
    int half = len >> 1, step = N / len;
    for (int i = 0; i < N; i += len) {
      for (int k = 0; k < half; k++) {
        float wr = fftCos[k * step], wi = fftSin[k * step];
        int a = i + k, b = a + half;
        float tr = fftRe[b] * wr - fftIm[b] * wi;
        float ti = fftRe[b] * wi + fftIm[b] * wr;
        fftRe[b] = fftRe[a] - tr; fftIm[b] = fftIm[a] - ti;
        fftRe[a] += tr;           fftIm[a] += ti;
      }
    }
  }
}

void specReset() {
  tft.fillRect(0, 98, 320, 50, TFT_BLACK);
  for (int b = 0; b < SPEC_BARS; b++) { specLevel[b] = 0; specPeak[b] = 0; specPeakHold[b] = 0; }
}

// h, ph : バーとピークの高さ (ピクセル)。下から 緑 → 黄 → 赤
void drawSpecBar(int b, int h, int ph) {
  const int x = SPEC_X + b * SPEC_PITCH, bottom = SPEC_Y + SPEC_H;
  if (SPEC_H - h > 0) tft.fillRect(x, SPEC_Y, SPEC_BAR_W, SPEC_H - h, TFT_BLACK);
  const int g = SPEC_H * 60 / 100, y2 = SPEC_H * 85 / 100;
  const int segLo[3] = {0, g, y2}, segHi[3] = {g, y2, SPEC_H};
  const uint16_t segCol[3] = {TFT_GREEN, TFT_YELLOW, TFT_RED};
  for (int i = 0; i < 3; i++) {
    int hi = min(segHi[i], h);
    if (hi > segLo[i]) tft.fillRect(x, bottom - hi, SPEC_BAR_W, hi - segLo[i], segCol[i]);
  }
  if (ph > h && ph > 0) tft.fillRect(x, bottom - ph, SPEC_BAR_W, 2, TFT_WHITE);   // ピーク
}

void specUpdate() {
  static uint32_t last = 0;
  static bool wasIdle = false;
  uint32_t now = millis();
  if (now - last < 40) return;                     // 約25fps
  last = now;

  float target[SPEC_BARS];
  for (int b = 0; b < SPEC_BARS; b++) target[b] = 0;

  if (playing && specHead >= (uint32_t)FFT_N) {
    uint32_t fs = audio.getSampleRate();
    if (fs < 8000 || fs > 96000) fs = 44100;
    uint32_t head = specHead;
    for (int i = 0; i < FFT_N; i++) {
      fftRe[i] = (float)specBuf[(head - FFT_N + i) & (FFT_N - 1)] * (1.0f / 32768.0f) * fftWin[i];
      fftIm[i] = 0.0f;
    }
    fftRun();
    const float norm = 4.0f / FFT_N;               // ハン窓: 振幅1の正弦波で |X| = N/4
    for (int k = 0; k <= FFT_N / 2; k++) fftMag[k] = sqrtf(fftRe[k] * fftRe[k] + fftIm[k] * fftIm[k]) * norm;

    const float res  = (float)fs / FFT_N;
    const float fmax = min(SPEC_FMAX, fs * 0.45f);
    const float ratio = fmax / SPEC_FMIN;
    float db[SPEC_BARS], frameMaxDb = -120.0f;
    for (int b = 0; b < SPEC_BARS; b++) {
      float flo = SPEC_FMIN * powf(ratio, (float)b / SPEC_BARS);
      float fhi = SPEC_FMIN * powf(ratio, (float)(b + 1) / SPEC_BARS);
      float fc  = sqrtf(flo * fhi);
      int lo = (int)ceilf(flo / res), hi = (int)floorf(fhi / res);
      float m = 0.0f;
      if (hi >= lo && hi >= 1) {                   // バーの帯域に FFT の点が含まれる: その最大値
        lo = max(lo, 1); hi = min(hi, FFT_N / 2);
        for (int k = lo; k <= hi; k++) if (fftMag[k] > m) m = fftMag[k];
      } else {                                     // 帯域が細くて点がない (低域): 前後の点から補間
        float pos = fc / res;
        int i0 = constrain((int)pos, 0, FFT_N / 2 - 1);
        float fr = pos - (float)i0;
        m = fftMag[i0] * (1.0f - fr) + fftMag[i0 + 1] * fr;
      }
      db[b] = 20.0f * log10f(m + 1e-6f) + SPEC_TILT_DB_OCT * log2f(fc / 1000.0f);
      if (db[b] > frameMaxDb) frameMaxDb = db[b];
    }
    specDbMax = max(max(specDbMax - 0.3f, frameMaxDb), -50.0f);   // 自動ゲイン (約7dB/秒で戻る)
    for (int b = 0; b < SPEC_BARS; b++)
      target[b] = constrain((db[b] - (specDbMax - SPEC_RANGE_DB)) / SPEC_RANGE_DB, 0.0f, 1.0f);
  }

  int h[SPEC_BARS], ph[SPEC_BARS];
  float sum = 0.0f;
  for (int b = 0; b < SPEC_BARS; b++) {
    float& lv = specLevel[b];
    if (target[b] >= lv) lv = target[b];                         // 立ち上がりは即時
    else lv = max(target[b], lv - 0.07f);                        // 立ち下がりはゆっくり
    float& pk = specPeak[b];
    if (lv >= pk) { pk = lv; specPeakHold[b] = 12; }             // ピークホールド
    else if (specPeakHold[b] > 0) specPeakHold[b]--;
    else pk = max(lv, pk - 0.025f);
    h[b]  = (int)(lv * SPEC_H + 0.5f);
    ph[b] = (int)(pk * SPEC_H + 0.5f);
    sum += lv + pk;
  }
  if (sum == 0.0f && wasIdle) return;                            // 何も表示がなければ描画しない
  wasIdle = (sum == 0.0f);

  tft.startWrite();
  for (int b = 0; b < SPEC_BARS; b++) drawSpecBar(b, h[b], ph[b]);
  tft.endWrite();
}

void drawAll() {
  tft.fillScreen(TFT_BLACK);
  drawHeader(); drawStation(); drawTitle(); drawVolume();
#if ENABLE_SPECTRUM
  specReset();
#endif
  for (int i = 0; i < B_COUNT; i++) drawButton(i);
}

// ---------------- 再生制御 (UIタスク側から呼ぶ) ----------------
void startStation(int idx) {
  curStation = (idx + STATION_COUNT) % STATION_COUNT;
  streamTitle[0] = 0;
  playing = true;
  reqStation = curStation;
  prefsDirty = true; prefsDirtyAt = millis();
  drawHeader(); drawStation(); drawTitle(); drawButton(B_PLAY);
}

void stopPlayback() {
  playing = false;
  reqStop = true;
  streamTitle[0] = 0;
  drawHeader(); drawTitle(); drawButton(B_PLAY);
}

// 音量(0..21) → ゲイン。dB 単位で等間隔に下げるので、小さい音量側も細かく調整できる
void applyVolume() {
  if (volume <= 0) { gainQ15 = 0; return; }
  float db = VOLUME_MIN_DB + (float)(VOLUME_MAX_DB - VOLUME_MIN_DB) * (volume - 1) / 20.0f;
  gainQ15 = (int32_t)(powf(10.0f, db / 20.0f) * 32768.0f);
}

void setVol(int v) {
  volume = constrain(v, 0, 21);
  applyVolume();
  prefsDirty = true; prefsDirtyAt = millis();
  drawVolume();
}

// ---------------- 音声タスク (Core0) ----------------
void audioTask(void*) {
  uint32_t retryDelayMs = 2000;   // 再接続の待ち時間 (失敗が続くと 15 秒まで延ばす)
  uint32_t reconnectAt  = 0;      // 0 以外: この時刻に再接続する

  for (;;) {
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
  LOGF("info: %s\n", info);
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
void IRAM_ATTR audio_process_i2s(uint32_t* sample, bool* continueI2S) {
  int32_t l = (int16_t)(*sample >> 16);
  int32_t r = (int16_t)(*sample & 0xFFFF);


  // モノラル化 (スピーカー出力は GPIO26 の1本だけなので、L/R を混ぜる) + 音量
  int32_t m = (l + r) >> 1;
#if ENABLE_SPECTRUM
  // スペクトラム用に、音量を掛ける前の音声を記録する (音量に左右されない)
  specBuf[specHead & (FFT_N - 1)] = (int16_t)m;
  specHead = specHead + 1;
#endif
  m = (m * gainQ15) >> 15;

  // 内蔵DACは8bit。16bit → 8bit への丸め
#if AUDIO_OUT_I2S
  int32_t q = m;                                           // 外付けI2S(16bit)は丸め不要
#elif AUDIO_DITHER_MODE == 0
  int32_t q = (m + 128) >> 8 << 8;                         // 単純な丸め
#else
  int32_t d = 0;                                             // ディザ (モード3では使わない)
  #if AUDIO_DITHER_MODE != 3
  static uint32_t rng = 2463534242u;
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;       // xorshift32
  d = (int32_t)(rng & 0xFF) - 128;                           // ±半LSB (8bit換算)
  #endif
  #if AUDIO_DITHER_MODE >= 2
  static int32_t err = 0;
  int32_t w = m + err;                                       // 誤差を持ち越す (ノイズシェーピング)
  #else
  int32_t w = m;
  #endif
  int32_t q = ((w + d + 128) >> 8) << 8;
  #if AUDIO_DITHER_MODE >= 2
  err = w - q;
  if (err > 512)  err = 512;
  if (err < -512) err = -512;
  #endif
#endif
#if AUDIO_OUT_I2S
  if (q > 32767)  q = 32767;
#else
  if (q > 32512)  q = 32512;
#endif
  if (q < -32768) q = -32768;

  uint16_t u = (uint16_t)(int16_t)q;
  *sample = ((uint32_t)u << 16) | u;
  *continueI2S = true;       // true にしないと音が出ない
}
void audio_eof_stream(const char* s)      { LOGF("eof: %s\n", s); lastStartMs = 0; }

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
  WiFi.setSleep(WIFI_MODEM_SLEEP);
  WiFi.setTxPower(WIFI_TX_POWER);
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
  WiFi.setTxPower(WIFI_TX_POWER);
  LOGF("WiFi OK: %s (RSSI %d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
#if SERIAL_LOG
  Serial.begin(115200);
#else
  esp_log_level_set("*", ESP_LOG_NONE);   // I2S の信号線に UART0 (IO1/IO3) を使うので、ログ出力を止める
#endif
  LOGF("CPU %d MHz\n", getCpuFrequencyMhz());

  pinMode(21, OUTPUT);         // バックライトは PWM を使わず常時ON (PWMのノイズ混入を避ける)
  digitalWrite(21, HIGH);
  tft.init();
  tft.setRotation((CYD_LANDSCAPE + (SCREEN_FLIP ? 2 : 0)) & 3);   // 横向き 320x240
  tft.fillScreen(TFT_BLACK);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("radio", false);
  curStation = constrain(prefs.getInt("station", 0), 0, STATION_COUNT - 1);
  volume     = constrain(prefs.getInt("volume", DEFAULT_VOLUME), 0, 21);

  connectWiFi();
#if AUDIO_OUT_I2S
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
#endif
  audio.setBufsize(AUDIO_BUFFER_BYTES, 0);   // 最初の接続より前に呼ぶ必要がある
  g_audioPrebuffer = min<uint32_t>(AUDIO_PREBUFFER_BYTES, AUDIO_BUFFER_BYTES - 4000);
  audio.setVolume(21);     // ライブラリ側は最大(等倍)に固定。音量は audio_process_i2s() で調整
  applyVolume();

#if ENABLE_SPECTRUM
  specInit();
#endif
  drawAll();
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  startStation(curStation);
}

void loop() {
  handleTouch();
  // 2秒ごとに受信バッファの状況を表示 (途切れの原因調査用)
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 2000) {
    lastDbg = millis();
    uint32_t filled = audio.inBufferFilled(), freeB = audio.inBufferFree();
    LOGF("buf %u/%u bytes  RSSI %d dBm  heap %u\n",
                  filled, filled + freeB, WiFi.RSSI(), ESP.getFreeHeap());
  }
  if (prefsDirty && millis() - prefsDirtyAt > 3000) {      // 最後の操作から3秒後に保存
    prefsDirty = false;
    prefs.putInt("station", curStation);
    prefs.putInt("volume", volume);
  }
  if (titleDirty) { titleDirty = false; drawTitle(); }
#if ENABLE_SPECTRUM
  specUpdate();
#endif
  delay(5);
}
