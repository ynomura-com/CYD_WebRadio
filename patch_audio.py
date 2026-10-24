# ESP32-audioI2S 2.0.6 に「再生開始前のプリバッファ」を追加するパッチ (ビルド時に自動適用)
#
# 元のライブラリは、受信バッファに 1 フレーム (約1.6KB) 溜まった時点で再生を始めるため、
# 実時間と同じ速さで送られてくるラジオ配信では、バッファがほぼ空のままになり、
# 通信が少し遅れるだけで音が途切れる。
# このパッチで、グローバル変数 g_audioPrebuffer (バイト) 以上溜まるまで再生開始を待つようにする。
import glob
import os
import re

MARKER = "g_audioPrebuffer"
PATTERN = re.compile(r"InBuff\.bufferFilled\(\) > maxFrameSize && !f_stream")
REPLACE = ("InBuff.bufferFilled() > ((g_audioPrebuffer > (uint32_t)maxFrameSize) ? "
           "g_audioPrebuffer : (uint32_t)maxFrameSize) && !f_stream")


def patch_file(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        src = f.read()
    if MARKER in src:
        return "already"
    new, n = PATTERN.subn(REPLACE, src)
    if n == 0:
        return "notfound"
    # 変数の定義を #include "Audio.h" の直後に追加
    new, m = re.subn(r'(#include\s+"Audio\.h"[^\n]*\n)',
                     r'\1volatile uint32_t g_audioPrebuffer = 0;   // patch_audio.py\n',
                     new, count=1)
    if m == 0:
        return "noinclude"
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(new)
    return "patched(%d)" % n


def run(libdeps_dir):
    files = glob.glob(os.path.join(libdeps_dir, "*", "ESP32-audioI2S*", "src", "Audio.cpp"))
    if not files:
        print("[patch_audio] ESP32-audioI2S が見つかりません (初回ビルド時は、ライブラリ取得後にもう一度ビルドしてください)")
    for p in files:
        print("[patch_audio] %s : %s" % (p, patch_file(p)))


try:
    Import("env")          # PlatformIO から実行されたとき
    run(env.subst("$PROJECT_LIBDEPS_DIR"))
except NameError:          # 単体実行 (テスト用)
    import sys
    if len(sys.argv) > 1:
        print(patch_file(sys.argv[1]))
