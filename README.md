# SuperCamera

Usee PlusプロトコルのUSB内視鏡/ペリスコープカメラ（`3301:2001 Geek szitman supercamera`）を、専用アプリなしで使うためのツール群。

USBから直接フレームを読み取り、MJPEG HTTPストリームとして公開します。VLC・ブラウザ・Home Assistant・ffmpegから利用でき、RTSP変換を挟めばUniFi Protectにもthird-partyカメラとして登録できます。

## なぜ専用アプリが必要だったのか

このカメラはUVCではなく**独自USB bulkプロトコル**（Usee Plus / com.useeplus.protocol）で通信します。OS標準のカメラAPI（V4L2等）からは見えないため、専用アプリが必須でした。本リポジトリはこのプロトコルを直接実装し、標準的なMJPEGストリームに変換します。

## クイックスタート

```bash
# 依存関係
sudo apt-get install build-essential libusb-1.0-0-dev

# ビルド
g++ -std=c++20 -O2 -o supercamera_server supercamera_server.cpp -lusb-1.0 -pthread
g++ -std=c++20 -O2 -o supercamera_capture supercamera_capture.cpp -lusb-1.0 -pthread

# USB権限 (udev)
sudo cp deploy/99-supercamera.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# 起動
./supercamera_server 8080
```

ブラウザで `http://<host>:8080/` を開くとライブビューが表示されます。

### systemdサービスとして常駐させる場合

```bash
sudo cp deploy/supercamera.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now supercamera.service
```

> `deploy/supercamera.service` の `User=` / `WorkingDirectory=` / `ExecStart=` は環境に合わせて変更してください。

## エンドポイント

| URL | 説明 |
|---|---|
| `http://<host>:8080/` | HTMLビューア |
| `http://<host>:8080/stream` | MJPEGストリーム (multipart/x-mixed-replace) — VLC / Home Assistant / ffmpeg対応 |
| `http://<host>:8080/stream.raw` | 連続JPEGストリーム (multipartなし) |
| `http://<host>:8080/snapshot` | 単一JPEGフレーム |

## プロトコル解析

raw USB dumpの解析で特定したプロトコル仕様です。

- **VID:PID**: `3301:2001`、Interface 0 (EP 0x01/0x81) + Interface 1 (EP 0x02/0x82)
- **初期化シーケンス**:
  1. MFi probe `FF 55 FF 55 EE 10` → EP1 (0x01)
  2. Open stream `BB AA 05 00 00` → EP2 (0x02)
  3. Video read → EP2 (0x82)、1024B bulk転送
- **USB転送 = 1024B = 512Bブロック×2**
- 各512Bブロック: **12Bヘッダ + 500B JPEGデータ**
  - ヘッダ: `aa bb 07 fb 01` + fid(1B) + cam_num(1B) + cont(1B) + meta(4B)
- **フレーム境界 = EOI (FF D9)** — JPEGのバイトスタッフィング規則によりエントロピーデータ内にFF D9は出現しないため、EOI検出は正確
- EOI後は次のフレームのヘッダが0-3Bのパディングを挟んで開始
- 起動直後の最初のフレーム (fid=0) は不完全 — スキップする
- 解像度: 1280x720 JPEG、約15-17fps、フレームサイズ約17-23KB

### 落とし穴

- **EP配置は機種で異なる**: 既知の別機種 (2CE3:3828) は全通信EP1だが、3301:2001はMFi→EP1、open+video→EP2。動かない場合はraw dumpで確認すること
- **lengthフィールド (0x01fb=507) はパケットサイズと不整合** — 無視して全バイトを使う
- **cam_numはフレーム境界ではない** (後続フレームでは全パケット0)
- ショートパケット (<1024B) 単独ではフレーム境界として信頼できない
- 最初のフレームは壊れている — スキップ必須

## UniFi Protect統合 (RTSP Bridge)

MJPEGストリームをH.264 RTSPに変換し、ONVIFブリッジ経由でUniFi Protectにthird-partyカメラとして登録する構成です。

```
supercamera.service (MJPEG :8080)
  → ffmpeg (MJPEG→H.264 RTSP :8556)
  → ONVIFブリッジ (WS-Discovery + SOAP :8089) + MediaMTX
  → UniFi Protect「RTSP Bridge SuperCamera」
```

### 構成要素

| コンポーネント | 役割 |
|---|---|
| `supercamera.service` | MJPEG HTTPサーバー (port 8080) |
| `supercamera-rtsp.service` | ffmpegでMJPEG→H.264 RTSP変換 (port 8556) |
| `supercamera-protect.service` | ONVIFブリッジ (WS-Discovery + SOAP API, port 8089) + MediaMTX |

### セットアップ手順

```bash
# 1. ffmpeg + MediaMTX
sudo apt-get install ffmpeg
# MediaMTXバイナリを配置 (https://github.com/bluenviron/mediamtx/releases)

# 2. ONVIFブリッジ (Node.js)
#    action4-protectのブリッジ実装を流用:
#      dist/src/ をコピーし、環境変数でカメラ情報を設定
#      CAMERA_ID=supercamera CAMERA_NAME=SuperCamera
#      CAMERA_RTSP_URL=rtsp://127.0.0.1:8556/supercamera
#      CAMERA_PORT=8089 HOST_IP=<LAN IP> RTSP_HOST=<LAN IP> RTSP_STREAM_PORT=8556

# 3. ffmpeg変換 (multipart形式が安定)
ffmpeg -f mjpeg -i http://127.0.0.1:8080/stream \
  -c:v libx264 -preset ultrafast -tune zerolatency -profile:v high \
  -pix_fmt yuv420p -r 17 -g 17 -bf 0 -b:v 2000k \
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8556/supercamera

# 4. UniFi Protect DBにカメラ登録 (macvlan環境)
#    Protectコンテナから到達可能なIPでONVIFブリッジを公開する
#    (macvlanの場合はunifi-shim等のホストIPエイリアスが必要)
```

### 落とし穴 (UniFi Protect)

- **macvlanネットワークではホストのeth0 IPに到達できない** — Protectコンテナと同じネットワークから到達可能なIP (unifi-shim等) でブリッジを公開すること
- **`/stream.raw`はffmpegのMJPEGデコーダでエラーが出る** — multipart形式の`/stream`を使うと安定する
- snapshotはthird-partyカメラでは取得不可 (既知の制限)
- ChangeVideoSettingsの"No response"警告は全third-partyカメラ共通の既知挙動で、録画には影響しない

## 構成

| ファイル | 説明 |
|---|---|
| `supercamera_server.cpp` | MJPEG HTTPストリーミングサーバー (port 8080) |
| `supercamera_capture.cpp` | フレームキャプチャツール (JPEG保存) |
| `deploy/supercamera.service` | systemdサービス定義 |
| `deploy/99-supercamera.rules` | udevルール (USB権限 0666) |

## License

CC0 — プロトコル解析は [hbens/geek-szitman-supercamera](https://github.com/hbens/geek-szitman-supercamera) (CC0) と [Tibiaworx/usee-plus-camera](https://github.com/Tibiaworx/usee-plus-camera) の知見に基づく。
