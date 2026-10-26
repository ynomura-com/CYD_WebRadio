#pragma once
#include <Arduino.h>

// ===== WiFi =====
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"

// WiFi の送信出力 (電流のピークが小さくなり、スピーカーのノイズが減ることがある)
// 弱い順: WIFI_POWER_2dBm, 5dBm, 7dBm, 8_5dBm, 11dBm, 13dBm, 15dBm, 17dBm, 18_5dBm, 19dBm, 19_5dBm(既定)
// 家の中なら 8_5dBm でも十分。つながりにくければ 11dBm / 15dBm に上げる
#define WIFI_TX_POWER WIFI_POWER_8_5dBm

// WiFi のモデムスリープ: 0=常時受信(既定) 1=省電力 (ノイズの出方が変わることがある)
#define WIFI_MODEM_SLEEP 0

// ===== タッチ (XPT2046) : CYD は別SPI(VSPI)に接続 =====
#define TOUCH_CLK  25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS   33
#define TOUCH_IRQ  36

// ===== 受信バッファ =====
// ライブラリの既定は約8KB (128kbps で 0.4秒分) しかなく、WiFi の揺らぎだけで音が途切れる。
// 大きいほど途切れに強いが、その分メモリを使う (https の局は TLS で約50KB 使う)。
// 目安: 40000 → 約2.5秒分 (128kbps)。起動ログの free heap に余裕がなければ小さくする
#define AUDIO_BUFFER_BYTES 40000

// 再生を始める前に溜めておく量 (バイト)。patch_audio.py でライブラリに追加した機能。
// ライブ配信は実時間と同じ速さでしか届かないため、溜めずに再生を始めると、
// 通信が少し遅れただけで音が途切れる。128kbps なら 16000 バイト = 約1秒分。
// 大きいほど途切れに強いが、局を選んでから音が出るまでの時間が延びる。
// AUDIO_BUFFER_BYTES より小さい値にすること (自動で上限をかける)
#define AUDIO_PREBUFFER_BYTES 24000

// ===== CPUクロック =====
// 240 (既定) / 160 / 80 (MHz)。
// 下げるとスピーカーの「ピー」音は小さくなるが、MP3のデコードが間に合わなくなると、
// I2S が無音(DACでは 0V)を出力し、音量に関係なく大きな「ブツブツ」音が鳴る。
// 「ブツブツ」が出る場合は 240 に戻すこと
#define CPU_FREQ_MHZ 240

// ===== 音声出力 =====
// 0: 内蔵DAC (CYD のスピーカー端子 / GPIO26)
// 1: 外付けI2S DAC/アンプ (MAX98357A など) ※ ノイズが気になる場合はこちらが根本的な対策
#define AUDIO_OUT_I2S 0
// AUDIO_OUT_I2S=1 のときのピン (MAX98357A の BCLK / LRC / DIN につなぐ)
//
// 基板の拡張端子 (実機で確認済み):
//   CN1 : GND, IO22, IO27, 3V3
//   P3  : GND, IO35, IO22, IO21
// このうち I2S の出力に使えるのは IO22 と IO27 の2本だけ。
//   IO35 : 入力専用のピンなので、出力には使えない
//   IO21 : バックライトと共用なので使えない
// I2S は3本 (BCLK / LRC / DIN) 必要なので、3本目 (DIN) を次のどれかから選ぶ:
//   (A) IO1 : P1 (シリアル) コネクタの TX。半田付け不要。ただし UART0 と共用なのでシリアルログが使えなくなる
//   (B) IO16 / IO17 / IO4 : 基板の RGB LED の端子から配線 (半田付けが必要)。シリアルログも使える
// ※ 15(液晶CS)、25/32/33/36/39(タッチ)、2/12/13/14(液晶) は使えない
#define I2S_BCLK 27
#define I2S_LRC  22
#define I2S_DOUT 1

// シリアルログ (1:出す 0:出さない)。I2S のピンに IO1 / IO3 を使う場合は自動で 0 になる
#define SERIAL_LOG 1
#if AUDIO_OUT_I2S && (I2S_BCLK == 1 || I2S_BCLK == 3 || I2S_LRC == 1 || I2S_LRC == 3 || I2S_DOUT == 1 || I2S_DOUT == 3)
  #undef  SERIAL_LOG
  #define SERIAL_LOG 0
#endif

// CYDのスピーカー端子(アンプ入力)は GPIO26 = ESP32内蔵DAC2 に接続されている
// (GPIO25 はタッチのCLKと競合するため LEFT のみ使用)

#define DEFAULT_VOLUME 6    // 0..21

// ---- 音量カーブ ----
// 音量1 → VOLUME_MIN_DB、音量21 → VOLUME_MAX_DB まで、dB で等間隔に変化する (0 は消音)
// 最低音量でもまだ大きい: VOLUME_MIN_DB をもっと小さく (例 -55)
// 最大音量が小さい    : VOLUME_MAX_DB を 0 に近づける
// 音量1 → VOLUME_MIN_DB、音量21 → VOLUME_MAX_DB。
// -30 付近まで絞ると、音楽の振幅が DAC (8bit) の数段分しかなくなり、音楽が聞こえなくなる。
// 歪む場合は MAX を下げ (-16 など)、小さすぎる場合は上げる (-6 など)
#if AUDIO_OUT_I2S
  #define VOLUME_MAX_DB  -6     // 外付けI2S(16bit): 内蔵DACのような制約がないので、広く使える
  #define VOLUME_MIN_DB  -50
#else
  #define VOLUME_MAX_DB  -12
  #define VOLUME_MIN_DB  -46
#endif

// 内蔵DAC(8bit)への丸め方:
//   0 = 単純な丸め
//   1 = ディザのみ         (無音でも雑音が出るので非推奨)
//   2 = ディザ + ノイズシェーピング (無音でも雑音が出るので非推奨)
//   3 = ノイズシェーピングのみ (無音では雑音が出ない。量子化のざらつきを高い周波数へ逃がす)
#define AUDIO_DITHER_MODE 3

// スペクトラム表示: 1=表示 0=無効 (音切れが多いときは 0 を試す)
#define ENABLE_SPECTRUM 1

// 画面の上下が逆に見える場合は 1 にする (180度回転)
#define SCREEN_FLIP 0

struct Station {
  const char* name;
  const char* url;   // MP3/AAC ストリーム (http 推奨。https も動くが TLS でメモリを使う)
};

// 好きな局に書き換えてください (局名は日本語もOK / UTF-8)
const Station STATIONS[] = {
    {".977 Smooth Jazz", "http://29319.live.streamtheworld.com:3690/977_SMOOJAZZ_SC"},
    {"五反野 FM", "https://radio.gotanno.love/"},
    {"FM かほく, Ishikawa)", "http://radio.kahoku.net:8000/;"},
    {"Yggdrasil Radio", "http://yggdrasilradio.net:8000/"},
    {"LUXURIAMUSIC", "http://ice10.securenetsystems.net/LUXOMP3"},
    {"The Jazz Groove (West) [AAC]", "http://west-aac-64.streamthejazzgroove.com/stream"},
    {"Aloha Joe's Tiki Radio", "http://s2.voscast.com:9120/stream"},
    {"LuxuriaMusic  US [AAC]", "http://ice10.securenetsystems.net/LUXOTRON"},
    {"Hawaii Public Radio HPR-1 (KHPR)", "http://khpr-ice.streamguys1.com/khpr2"},
    {"The Jazz Groove - East", "http://east-mp3-128.streamthejazzgroove.com/stream"},
    {"Big Blue Swing US", "http://radio.bigblueswing.com:8002/"},
    {"KCSM Jazz 91.1 FM", "http://ice7.securenetsystems.net/KCSM2"},
    {"XRadio: The Worldwide Superstation SG", "http://xradio.zone:9420/stream.mp3"},
    {"thejazzgroove.org WEST", "http://west-mp3-128.streamthejazzgroove.com/;stream.mp3"},
    {"The Jazz Groove", "http://mp3-128.streamthejazzgroove.com/;stream.mp3"},
    {"KJZZ 91.5 Phoenix, AZ", "http://stream.kjzz.org/kjzz_mp3_128"},
    {"103.5 KISS FM WKSC Chicago", "http://stream.revma.ihrhls.com/zc849"},
    {"Y100 WHYI FM 100.7 Miami", "http://stream.revma.ihrhls.com/zc561"},
    {"96.7 KISS FM KHFI Austin", "http://stream.revma.ihrhls.com/zc1173"},
    {"KISS 108 WXKS FM 107.9 Boston", "http://stream.revma.ihrhls.com/zc1097"},
    {"102.7 KIIS FM Los Angeles", "http://stream.revma.ihrhls.com/zc185"},
    {"102.7 KISS FM WEGR Memphis", "http://stream.revma.ihrhls.com/zc2133"},
    {"Z100 WHTZ FM 100.3 New York", "http://stream.revma.ihrhls.com/zc1469"},
    {"Soft Classic Rock", "http://64.71.133.122:8000/"},
    {"boogaloostereo", "http://149.56.157.81:8399/"},
};
const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
