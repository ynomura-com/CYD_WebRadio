// ノイズ診断用 (env:noise_test)
// 音声は一切再生せず、DAC を無音(中点)に固定したまま、
// 「何を動かすとザー音が出るか」をシリアルコマンドで1つずつ切り替えて確認する。
#include <Arduino.h>
#include <WiFi.h>
#include "LGFX_CYD.h"
#include "config.h"

LGFX tft;
bool backlightOn = false, dispOn = false, dispInit = false;
bool wifiOn = false, netOn = false, dacOn = true, burnOn = false;
volatile bool burnFlag = false;
bool sleepOn = false;      // WiFi モデムスリープ
int  cpuIdx = 0;           // CPUクロックの段階
int  dacIdx = 0;           // DAC 無音レベルの段階
const int DAC_LEVEL[] = {128, 96, 64, 32, 16, 0};
const int CPU_MHZ[] = {240, 160, 80};
int  txIdx = 2;            // 送信出力の段階
const wifi_power_t TX_LEVEL[] = {WIFI_POWER_2dBm, WIFI_POWER_5dBm, WIFI_POWER_8_5dBm,
                                 WIFI_POWER_11dBm, WIFI_POWER_15dBm, WIFI_POWER_19_5dBm};
const char* TX_NAME[] = {"2dBm", "5dBm", "8.5dBm", "11dBm", "15dBm", "19.5dBm"};
WiFiClient net;
uint32_t lastDraw = 0, lastRx = 0, rxBytes = 0;

void status() {
  Serial.printf("\n[状態] DAC無音出力:%s  バックライト:%s  画面更新:%s  WiFi:%s  通信負荷:%s  CPU負荷:%s\n",
                dacOn ? "ON" : "OFF(ピン解放)", backlightOn ? "ON" : "OFF",
                dispOn ? "ON" : "OFF", wifiOn ? "ON" : "OFF", netOn ? "ON" : "OFF",
                burnOn ? "ON" : "OFF");
  Serial.printf("[WiFi設定] モデムスリープ:%s  送信出力:%s  CPUクロック:%dMHz  DACレベル:%d\n", sleepOn ? "ON" : "OFF", TX_NAME[txIdx], getCpuFrequencyMhz(), DAC_LEVEL[dacIdx]);
}

void help() {
  Serial.println("\n=== CYD ノイズ診断 ===");
  Serial.println("スピーカーに耳を近づけ、1つずつ切り替えて「ザー音」が変わるか聞いてください。");
  Serial.println("  a : DAC(GPIO26)の無音出力 ON/OFF");
  Serial.println("  b : バックライト ON/OFF");
  Serial.println("  d : 画面の更新 ON/OFF (VUメーター相当の描画を30fpsで)");
  Serial.println("  w : WiFi 接続 ON/OFF");
  Serial.println("  n : WiFi 通信負荷 ON/OFF (ラジオ受信と同じ種類の通信)");
  Serial.println("  c : CPU負荷 ON/OFF (MP3デコード相当の計算を Core0 で)");
  Serial.println("  s : WiFi モデムスリープ ON/OFF (WiFi接続中に切り替えて聞き比べる)");
  Serial.println("  p : WiFi 送信出力を順に切り替え 2→5→8.5→11→15→19.5dBm (WiFi接続中に)");
  Serial.println("  f : CPUクロック切替 240→160→80MHz (音の高さが変わるか聞く)");
  Serial.println("  v : DAC の無音レベル切替 128→96→64→32→16→0 (下げるとノイズが減るか聞く)");
  Serial.println("  ? : この説明と現在の状態");
  status();
}

void setWifi(bool on) {
  if (on) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(sleepOn);
    WiFi.setTxPower(TX_LEVEL[txIdx]);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    wifiOn = (WiFi.status() == WL_CONNECTED);
    Serial.println(wifiOn ? "WiFi 接続OK" : "WiFi 接続失敗");
  } else {
    net.stop(); netOn = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiOn = false;
  }
}

// MP3デコード相当の負荷: 約1ms計算 → 約1ms休み を繰り返す (Core0)
void burnTask(void*) {
  volatile float x = 0.5f;
  for (;;) {
    if (burnFlag) {
      uint32_t t0 = micros();
      while (micros() - t0 < 1000) x = sinf(x) * 1.0001f + 0.3f;
    }
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(burnTask, "burn", 4096, nullptr, 1, nullptr, 0);
  delay(300);
  // RGB LED(4,16,17)は消灯。バックライトは消灯から開始
  for (int p : {4, 16, 17}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  pinMode(21, OUTPUT); digitalWrite(21, LOW);
  dacWrite(26, 128);
  help();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'a': dacOn = !dacOn; if (dacOn) dacWrite(26, 128); else dacDisable(26); status(); break;
      case 'b': backlightOn = !backlightOn; digitalWrite(21, backlightOn); status(); break;
      case 'd':
        dispOn = !dispOn;
        if (dispOn && !dispInit) {
          tft.init(); tft.setRotation((CYD_LANDSCAPE + (SCREEN_FLIP ? 2 : 0)) & 3);
          tft.fillScreen(TFT_BLACK); dispInit = true;
        }
        status(); break;
      case 'w': setWifi(!wifiOn); status(); break;
      case 'n':
        if (!wifiOn) { Serial.println("先に w で WiFi を接続してください"); break; }
        netOn = !netOn;
        if (!netOn) net.stop();
        status(); break;
      case 's': sleepOn = !sleepOn; WiFi.setSleep(sleepOn); status(); break;
      case 'p': txIdx = (txIdx + 1) % 6; WiFi.setTxPower(TX_LEVEL[txIdx]); status(); break;
      case 'v': dacIdx = (dacIdx + 1) % 6; dacOn = true; dacWrite(26, DAC_LEVEL[dacIdx]); status(); break;
      case 'f': cpuIdx = (cpuIdx + 1) % 3; setCpuFrequencyMhz(CPU_MHZ[cpuIdx]); status(); break;
      case 'c': burnOn = !burnOn; burnFlag = burnOn; status(); break;
      case '?': help(); break;
      default: break;
    }
  }

  // 画面更新 (VUメーターと同程度の描画)
  if (dispOn && millis() - lastDraw > 33) {
    lastDraw = millis();
    for (int i = 0; i < 24; i++)
      tft.fillRect(24 + i * 12, 129, 10, 7, (random(2) ? TFT_GREEN : 0x2104));
  }

  // 通信負荷: 128kbps のラジオ配信を受信し続ける (中身は捨てる)
  if (netOn) {
    if (!net.connected()) {
      net.stop();
      if (net.connect("ice1.somafm.com", 80)) {
        net.print("GET /groovesalad-128-mp3 HTTP/1.0\r\nHost: ice1.somafm.com\r\n\r\n");
      }
      delay(500);
    }
    uint8_t buf[512];
    int n = net.read(buf, sizeof(buf));
    if (n > 0) rxBytes += n;
    if (millis() - lastRx > 5000) { lastRx = millis(); Serial.printf("受信 %u bytes\n", rxBytes); }
  }
  delay(1);
}
