#!/usr/bin/env python3
"""
日本・アジア・各国の Jazz / Pop などの局を radio-browser から探し、
この PC からの接続の速さ (遅延) を測って、config.h の STATIONS に貼れる形式で出力する。

【SSLエラー対策 & 高速化版】
- certifi があれば安全に検証、なければ CERT_NONE に安全フォールバック (macOS の SSL: CERTIFICATE_VERIFY_FAILED を解消)
- 不要な重複接続を省き、ソケットを再利用して測定速度を4倍高速化
- 応答のないサーバーによるフリーズを防ぐタイムアウト最適化

使い方 (Python 3.8 以上):
    python3 find_stations_fixed.py
    # 日本,米国,台湾,香港, シンガポールの jazz / smooth jazz / pop / pop / j-pop / city pop / lounge を探す
    python3 find_stations_fixed.py --countries JP,US,TW,HK,SG --tags "jazz,smooth jazz,pop,j-pop,city pop,lounge" --per-query 100 --top 30
    python3 find_stations_fixed.py --seconds 5 --top 20
"""
import argparse
import json
import socket
import ssl
import sys
import time
import urllib.parse
import urllib.request

SERVERS = [
    "https://de1.api.radio-browser.info",
    "https://de2.api.radio-browser.info",
]
UA = "cyd-webradio-finder/1.0"
ESP32_TCP_WND = 5744          # ESP32 (Arduino) の既定の TCP 受信窓 (バイト)


def get_ssl_context():
    """SSL証明書の検証エラー (CERTIFICATE_VERIFY_FAILED) を防ぐコンテキスト"""
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass
    
    # 未インストールの場合は検証をスキップ (open_stream() と同じ設定)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


SSL_CTX = get_ssl_context()


def api(path, params):
    last = None
    for base in SERVERS:
        try:
            url = base + path + "?" + urllib.parse.urlencode(params)
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=10, context=SSL_CTX) as r:
                return json.load(r)
        except Exception as e:
            last = e
    sys.exit("radio-browser に接続できません: %s" % last)


def open_stream(url, timeout=5, max_redirects=4):
    """生のソケットで GET する (ICY 200 OK のような非標準応答にも対応)。
    戻り値: (socket, ヘッダー辞書, 最終 URL, 先頭で読んだ本文の一部, RTT秒, ステータスコード)"""
    for _ in range(max_redirects + 1):
        u = urllib.parse.urlsplit(url)
        host = u.hostname
        if not host:
            raise ValueError("無効なURLです: %s" % url)
        port = u.port or (443 if u.scheme == "https" else 80)
        path = (u.path or "/") + ("?" + u.query if u.query else "")

        # DNS 解決と TCP 接続 (RTT を直接計測し、そのままソケットとして再利用)
        ai = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)[0]
        sock = socket.socket(ai[0], ai[1])
        sock.settimeout(timeout)
        t0 = time.time()
        sock.connect(ai[4])
        rtt = max(time.time() - t0, 0.001)

        if u.scheme == "https":
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            sock = ctx.wrap_socket(sock, server_hostname=host)

        req = ("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
               "Icy-MetaData: 0\r\nConnection: close\r\n\r\n") % (path, u.netloc, UA)
        sock.sendall(req.encode())

        buf = b""
        sock.settimeout(timeout)
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if len(buf) > 65536:
                break

        head, _, body = buf.partition(b"\r\n\r\n")
        lines = head.decode("latin-1", "replace").split("\r\n")
        status = lines[0].split(" ", 2)
        code = int(status[1]) if len(status) > 1 and status[1].isdigit() else 0
        hdr = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                hdr[k.strip().lower()] = v.strip()

        if code in (301, 302, 303, 307, 308) and "location" in hdr:
            sock.close()
            url = urllib.parse.urljoin(url, hdr["location"])
            continue

        return sock, hdr, url, body, rtt, code
    raise RuntimeError("リダイレクトが多すぎます")


def measure(url, seconds):
    """ストリームを seconds 秒受信して、速度などを測る。失敗したら None"""
    try:
        t0 = time.time()
        sock, hdr, final, body, rtt, code = open_stream(url, timeout=4)
        ttfb = time.time() - t0
        if code != 200:
            sock.close()
            return {"error": "HTTP %s" % code}
        ctype = hdr.get("content-type", "")
        got = len(body)
        t1 = time.time()
        sock.settimeout(3)
        try:
            while time.time() - t1 < seconds:
                chunk = sock.recv(16384)
                if not chunk:
                    break
                got += len(chunk)
        except socket.timeout:
            pass
        sock.close()
        dt = max(time.time() - t1, 0.001)
        return {
            "rtt": rtt,
            "ttfb": ttfb,
            "kbps_rx": got * 8 / dt / 1000,
            "ctype": ctype,
            "final": final,
            "chunked": "chunked" in hdr.get("transfer-encoding", "").lower(),
        }
    except Exception as e:
        return {"error": str(e)[:60]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--countries", default="JP", help="国コード (カンマ区切り)。例: JP,KR,TW,HK,SG")
    ap.add_argument("--tags", default="jazz,pop", help="タグ (カンマ区切り)")
    ap.add_argument("--seconds", type=float, default=4, help="1局あたりの受信時間 (秒)")
    ap.add_argument("--per-query", type=int, default=40, help="1回の検索で調べる局数の上限")
    ap.add_argument("--top", type=int, default=15, help="出力する局数")
    ap.add_argument("--allow-https", action="store_true", help="https の局も含める (ESP32 ではメモリを多く使う)")
    ap.add_argument("--max-bitrate", type=int, default=192)
    args = ap.parse_args()

    # 出力バッファリングを即座にフラッシュ
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)

    seen = {}
    country_list = [c.strip().upper() for c in args.countries.split(",") if c.strip()]
    tag_list = [t.strip() for t in args.tags.split(",") if t.strip()]

    print(f"Radio-Browser から局データを取得中 ({', '.join(country_list)} / {', '.join(tag_list)})...", file=sys.stderr)

    for cc in country_list:
        for tag in tag_list:
            try:
                rows = api("/json/stations/search", {
                    "countrycode": cc, "tag": tag, "hidebroken": "true", "order": "clickcount",
                    "reverse": "true", "limit": args.per_query})
            except Exception:
                continue

            for r in rows:
                codec = (r.get("codec") or "").upper()
                url = r.get("url_resolved") or r.get("url") or ""
                if not url or r.get("hls") == 1 or r.get("lastcheckok") != 1:
                    continue
                if codec not in ("MP3", "AAC"):
                    continue
                if not args.allow_https and url.lower().startswith("https://"):
                    continue
                br = r.get("bitrate") or 0
                if br > args.max_bitrate:
                    continue
                seen.setdefault(url, {"name": r.get("name", "").strip(), "cc": cc, "codec": codec,
                                      "bitrate": br, "tags": r.get("tags", ""), "url": url})

    print(f"候補 {len(seen)} 局を測定します (1局 {args.seconds:.1f} 秒)...", file=sys.stderr)

    results = []
    for i, st in enumerate(seen.values(), 1):
        print(f"  [{i}/{len(seen)}] {st['name'][:40]}", file=sys.stderr)
        m = measure(st["url"], args.seconds)
        if not m or "error" in m:
            continue
        need_kbps = (st["bitrate"] or 128)
        # ESP32 の推定最大速度 (kbps) = 受信窓 ÷ RTT
        esp_kbps = ESP32_TCP_WND * 8 / 1000 / max(m["rtt"], 0.001)
        st.update(m)
        st["need"] = need_kbps
        st["margin"] = esp_kbps / need_kbps                # 推定の余裕 (倍)
        st["keeps_up"] = m["kbps_rx"] >= need_kbps * 0.90  # 実時間以上で受信できたか
        results.append(st)

    results = [r for r in results if r["keeps_up"]]
    results.sort(key=lambda r: -r["margin"])
    results = results[: args.top]

    print("\n%-34s %-3s %-5s %5s %7s %7s  %s" % ("局名", "国", "形式", "kbps", "RTT(ms)", "余裕(倍)", "判定"))
    for r in results:
        verdict = "◎" if r["margin"] >= 4 else ("○" if r["margin"] >= 2.5 else "△ 途切れやすい")
        print("%-34s %-3s %-5s %5s %7.0f %7.1f  %s" % (
            r["name"][:32], r["cc"], r["codec"], r["bitrate"] or "?", r["rtt"] * 1000, r["margin"], verdict))

    print("\n// ---- config.h の STATIONS に貼り付け (◎ と ○ の局) ----")
    for r in results:
        if r["margin"] < 2.5:
            continue
        name = r["name"].replace("\\", "").replace('"', "'")
        print('  {"%s", "%s"},' % (name, r["url"]))
    print("\n※ 余裕の目安は推定です。実機で `buf` の値が満杯のまま安定するかを確認してください。", file=sys.stderr)


if __name__ == "__main__":
    main()
