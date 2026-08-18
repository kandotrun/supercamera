# SuperCamera — Usee Plus USB Endoscope Camera (3301:2001)

NASに接続したUsee PlusプロトコルのUSB内視鏡/ペリスコープカメラ（`3301:2001 Geek szitman supercamera`）を、専用アプリなしで使うためのツール群。

## なぜ専用アプリが必要だったのか

このカメラはUVCではなく**独自USB bulkプロトコル**（Usee Plus / com.useeplus.protocol）を使用するため、OS標準のカメラAPI（V4L2等）からは見えません。専用アプリはこのプロトコルを直接叩いています。

## プロトコル概要（raw dump解析で特定）

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

## 構成

| ファイル | 説明 |
|---|---|
| `supercamera_server.cpp` | MJPEG HTTPストリーミングサーバー (port 8080) |
| `supercamera_capture.cpp` | フレームキャプチャツール (JPEG保存) |
| `deploy/supercamera.service` | systemdサービス定義 |
| `deploy/99-supercamera.rules` | udevルール (USB権限 0666) |

## ビルド

```bash
sudo apt-get install build-essential libusb-1.0-0-dev
g++ -std=c++20 -O2 -o supercamera_server supercamera_server.cpp -lusb-1.0 -pthread
g++ -std=c++20 -O2 -o supercamera_capture supercamera_capture.cpp -lusb-1.0 -pthread
```

## デプロイ (NAS / Linux)

```bash
# 1. udevルール (USB権限)
sudo cp deploy/99-supercamera.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# 2. ビルド
g++ -std=c++20 -O2 -o supercamera_server supercamera_server.cpp -lusb-1.0 -pthread

# 3. systemdサービス
sudo cp deploy/supercamera.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now supercamera.service
```

## エンドポイント

| URL | 説明 |
|---|---|
| `http://<host>:8080/` | HTMLビューア |
| `http://<host>:8080/stream` | MJPEGストリーム (multipart/x-mixed-replace) — VLC / Home Assistant対応 |
| `http://<host>:8080/snapshot` | 単一JPEGフレーム |

## 落とし穴

- **EP配置は機種で異なる**: 既知の別機種 (2CE3:3828) は全通信EP1だが、3301:2001はMFi→EP1、open+video→EP2。動かない場合はraw dumpで確認すること
- **lengthフィールド (0x01fb=507) はパケットサイズと不整合** — 無視して全バイトを使う
- **cam_numはフレーム境界ではない** (後続フレームでは全パケット0)
- ショートパケット (<1024B) 単独ではフレーム境界として信頼できない
- 最初のフレームは壊れている — スキップ必須

## License

CC0 — プロトコル解析は [hbens/geek-szitman-supercamera](https://github.com/hbens/geek-szitman-supercamera) (CC0) と [Tibiaworx/usee-plus-camera](https://github.com/Tibiaworx/usee-plus-camera) の知見に基づく。
