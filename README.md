# CYD Web Radio (ESP32-2432S028R + PlatformIO)

![](images/CYD_WebRadioS.jpeg)

## 使い方
1. VS Code で `cyd-webradio` フォルダを開く (PlatformIO IDE 拡張が必要)
2. `include/config.h` の `WIFI_SSID` / `WIFI_PASSWORD` を編集
3. スピーカーを CYD の `SPEAK` 端子 (P4, 1.25mm 2pin) に接続 *（画像はステレオ表示ですが、モノラルです）*
4. USB接続 → PlatformIO の Upload (→) → Monitor
初回のビルドで、ライブラリの取得と `patch_audio.py` によるパッチ適用が行われる。
ビルドログに `[patch_audio] ... patched(4)` または `already` と出れば適用済み。

## ファイル構成
```
platformio.ini      ビルド設定 (env:cyd が本体 / env:noise_test はノイズ診断用)
patch_audio.py      ライブラリに「再生前のプリバッファ」を追加するパッチ (platformio.ini と同じ場所に置く)
include/config.h    WiFi・局リスト・音量・ノイズ対策などの設定
include/LGFX_CYD.h  液晶 (ILI9342系) の設定
src/main.cpp        本体
src/noise_test.cpp  ノイズ診断用 (シリアルモニタからキー操作)
```

## 操作 (タッチ)
`|<` 前の局 / 再生・停止 / `>|` 次の局 / `-` `+` 音量
最後に聴いた局と音量は保存されます。

## ラジオ局の設定
[Radio BrowserでJapanを検索](https://www.radio-browser.info/search?page=1&order=clickcount&reverse=true&hidebroken=true&countrycode=JP})
などで曲の名前とURLを試してみて、`include/config.h` の `const Station STATIONS[] ` の配列を書き換えます。

## 動作確認済みの設定 (基板による違いもあり)
| 項目 | 設定 | 備考 |
|---|---|---|
| 液晶 | `CYD_PANEL 2` (ILI9342) / `invert = true` / `rgb_order = false` | `LGFX_CYD.h` |
| 向き | `SCREEN_FLIP 0` | 上下逆なら 1 |
| CPU | `CPU_FREQ_MHZ 240` | 下げると「ブツブツ」が出ることがある |
| 受信バッファ | `AUDIO_BUFFER_BYTES 40000` / `AUDIO_PREBUFFER_BYTES 24000` | 音切れ対策 |
| 丸め処理 | `AUDIO_DITHER_MODE 3` | ノイズシェーピングのみ。ノイズが大幅に改善 |
| 音量 | `VOLUME_MAX_DB -12` / `VOLUME_MIN_DB -46` | 内蔵DAC(8bit)向けの細かい音量カーブ |

## わかったこと (トラブルシュート)
| 症状 | 原因と対処 |
|---|---|
| 音が途切れる | ライブ配信は実時間の速さでしか届かず、元のライブラリは溜めずに再生を始めるため。`patch_audio.py` のプリバッファで解消 |
| 音量に関係なく大きな「ブツブツ」 | デコードが間に合わず I2S が無音(DACでは0V)を出している。`CPU_FREQ_MHZ` を 240 に |
| 音量ゼロでも「ザー」 | ディザ (`AUDIO_DITHER_MODE` 1,2) が雑音の原因。3 か 0 にする |
| 音が歪む・ざらつく | 内蔵DACは8bit。`AUDIO_DITHER_MODE 3` が有効。高い音量ではアンプ側も歪むので `VOLUME_MAX_DB` を下げる |
| 低い/高い一定の音 (WiFi接続時) | 基板の電源・回路由来。ソフトの設定では変化なし。外付けI2Sアンプで解消できる |
| 音楽が聞こえず「ジー」だけ | `VOLUME_MAX_DB` を絞りすぎ (-30付近) か、DACの中心レベルを下げた場合。中心レベルは変更しない |
| 局に繋がらない・402/401 | 配信側の制限。`再接続中: HTTP/...` と表示される。無理に使わず別の局に差し替える |
| 画面の色・向きがおかしい | `LGFX_CYD.h` の `invert` / `rgb_order` / `CYD_PANEL`、`config.h` の `SCREEN_FLIP` |

## ノイズ診断
`env:noise_test` を書き込み、シリアルモニタでキーを送る。音声は再生せず、DAC を無音に固定したまま、
画面更新・WiFi・CPU負荷・WiFi設定・CPUクロック・DACレベルを1つずつ切り替えて、ノイズの原因を切り分けられる。
診断が終わったら、ステータスバーで `env:cyd` に戻すこと。

## さらに音質を上げたい場合
内蔵DAC (8bit) とアンプの限界があるため、根本的な改善には外付けのI2S DAC/アンプ (MAX98357A など) が必要。
`config.h` の `AUDIO_OUT_I2S` を 1 にし、`I2S_BCLK` / `I2S_LRC` / `I2S_DOUT` を実際に引き出せるピンに合わせる。
