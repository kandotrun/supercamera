# SuperCamera

Tools for using a Usee Plus protocol USB endoscope/periscope camera (`3301:2001 Geek szitman supercamera`) without its proprietary app.

Reads frames directly over USB and serves them as an MJPEG HTTP stream. Works with VLC, browsers, Home Assistant, and ffmpeg — and with an RTSP conversion step, it can be registered in UniFi Protect as a third-party camera.

## Why the proprietary app was required

This camera does not use UVC. It speaks a **proprietary USB bulk protocol** (Usee Plus / com.useeplus.protocol), so it is invisible to standard OS camera APIs (V4L2, etc.). The vendor app talks this protocol directly. This repository implements the protocol and converts it into a standard MJPEG stream.

## Quick start

```bash
# Dependencies
sudo apt-get install build-essential libusb-1.0-0-dev

# Build
g++ -std=c++20 -O2 -o supercamera_server supercamera_server.cpp -lusb-1.0 -pthread
g++ -std=c++20 -O2 -o supercamera_capture supercamera_capture.cpp -lusb-1.0 -pthread

# USB permissions (udev)
sudo cp deploy/99-supercamera.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# Run
./supercamera_server 8080
```

Open `http://<host>:8080/` in a browser for the live view.

### Running as a systemd service

```bash
sudo cp deploy/supercamera.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now supercamera.service
```

> Adjust `User=`, `WorkingDirectory=`, and `ExecStart=` in `deploy/supercamera.service` to match your environment.

## Endpoints

| URL | Description |
|---|---|
| `http://<host>:8080/` | HTML viewer |
| `http://<host>:8080/stream` | MJPEG stream (multipart/x-mixed-replace) — VLC / Home Assistant / ffmpeg |
| `http://<host>:8080/stream.raw` | Consecutive JPEG stream (no multipart framing) |
| `http://<host>:8080/snapshot` | Single JPEG frame |

## Protocol analysis

Protocol specification reverse-engineered from raw USB dumps.

- **VID:PID**: `3301:2001`, Interface 0 (EP 0x01/0x81) + Interface 1 (EP 0x02/0x82)
- **Init sequence**:
  1. MFi probe `FF 55 FF 55 EE 10` → EP1 (0x01)
  2. Open stream `BB AA 05 00 00` → EP2 (0x02)
  3. Video read → EP2 (0x82), 1024-byte bulk transfers
- **USB transfer = 1024B = 2 × 512B blocks**
- Each 512B block: **12B header + 500B JPEG data**
  - Header: `aa bb 07 fb 01` + fid(1B) + cam_num(1B) + cont(1B) + meta(4B)
- **Frame boundary = EOI (FF D9)** — JPEG byte-stuffing rules guarantee FF D9 never appears inside entropy-coded data, so EOI detection is exact
- After EOI, the next frame's header starts after 0-3 bytes of padding
- The first frame after device startup (fid=0) is incomplete — skip it
- Resolution: 1280x720 JPEG, ~15-17 fps, frame size ~17-23 KB

### Pitfalls

- **Endpoint layout varies by model**: a known sibling model (2CE3:3828) uses EP1 for everything, but the 3301:2001 uses EP1 for MFi and EP2 for open+video. If it doesn't work, capture a raw dump and check.
- **The length field (0x01fb=507) is inconsistent with the packet size** — ignore it and use all bytes
- **cam_num is not a frame boundary** (it is 0 in all packets of subsequent frames)
- A short packet (<1024B) alone is not a reliable frame boundary
- The first frame is corrupted — skipping it is mandatory

## UniFi Protect integration (RTSP Bridge)

Converts the MJPEG stream to H.264 RTSP and registers it in UniFi Protect as a third-party camera via an ONVIF bridge.

```
supercamera.service (MJPEG :8080)
  → ffmpeg (MJPEG→H.264 RTSP :8556)
  → ONVIF bridge (WS-Discovery + SOAP :8089) + MediaMTX
  → UniFi Protect "RTSP Bridge SuperCamera"
```

### Components

| Component | Role |
|---|---|
| `supercamera.service` | MJPEG HTTP server (port 8080) |
| `supercamera-rtsp.service` | ffmpeg MJPEG→H.264 RTSP transcoder (port 8556) |
| `supercamera-protect.service` | ONVIF bridge (WS-Discovery + SOAP API, port 8089) + MediaMTX |

### Setup

```bash
# 1. ffmpeg + MediaMTX
sudo apt-get install ffmpeg
# Place the MediaMTX binary (https://github.com/bluenviron/mediamtx/releases)

# 2. ONVIF bridge (Node.js)
#    Reuse the action4-protect bridge implementation:
#      copy dist/src/ and configure camera info via environment variables
#      CAMERA_ID=supercamera CAMERA_NAME=SuperCamera
#      CAMERA_RTSP_URL=rtsp://127.0.0.1:8556/supercamera
#      CAMERA_PORT=8089 HOST_IP=<LAN IP> RTSP_HOST=<LAN IP> RTSP_STREAM_PORT=8556

# 3. ffmpeg transcoding (the multipart endpoint is the stable one)
ffmpeg -f mjpeg -i http://127.0.0.1:8080/stream \
  -c:v libx264 -preset ultrafast -tune zerolatency -profile:v high \
  -pix_fmt yuv420p -r 17 -g 17 -bf 0 -b:v 2000k \
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8556/supercamera

# 4. Register the camera in the UniFi Protect DB (macvlan environments)
#    Publish the ONVIF bridge on an IP reachable from the Protect container
#    (with macvlan, a host IP alias such as unifi-shim is required)
```

### Pitfalls (UniFi Protect)

- **On macvlan networks the host's eth0 IP is unreachable** — publish the bridge on an IP reachable from the Protect container's network (e.g. unifi-shim)
- **`/stream.raw` triggers errors in ffmpeg's MJPEG decoder** — use the multipart `/stream` endpoint for stability
- Snapshots are unavailable for third-party cameras (known limitation)
- The ChangeVideoSettings "No response" warning is a known behavior common to all third-party cameras and does not affect recording

## Files

| File | Description |
|---|---|
| `supercamera_server.cpp` | MJPEG HTTP streaming server (port 8080) |
| `supercamera_capture.cpp` | Frame capture tool (saves JPEGs) |
| `deploy/supercamera.service` | systemd service unit |
| `deploy/99-supercamera.rules` | udev rule (USB permissions 0666) |

## License

CC0 — protocol analysis builds on findings from [hbens/geek-szitman-supercamera](https://github.com/hbens/geek-szitman-supercamera) (CC0) and [Tibiaworx/usee-plus-camera](https://github.com/Tibiaworx/usee-plus-camera).
