
# W-Series Firmware — Sprint W0→W1 (Skeleton + UI Toggle)

**Tujuan:** mulai dari awal (incremental, aman):
- W0: Skeleton HTTP/WS + UI read-only (serve dari LittleFS), SoftAP.
- W1: Config runtime + endpoint POST (sensors/backend/guards/auto-trigger AC), NVS persist.

## Struktur
- `src/main.cpp` — server, endpoints, SoftAP, dummy WS telemetry.
- `include/config.h` — SSID/pass, FW ID.
- `data/` — Web UI (salinan UI v2, tool‑tips + Help/Tutorial).
- `platformio.ini` — deps: ESP Async WebServer, AsyncTCP, LittleFS_esp32, ArduinoJson.

## Build & Upload
```bash
# Upload LittleFS (UI)
pio run -t uploadfs

# Upload firmware
pio run -t upload

# Serial monitor
pio device monitor -b 115200
```

## Akses
- Sambung ke Wi‑Fi AP: SSID `WSeries-<MAC4>`, pass `wseries123`.
- Buka `http://192.168.4.1/`.

## Catatan
- WebSocket telemetry masih dummy (FSM belum dibuat).
- Endpoint `GET /api/config` dan `POST /api/config/*` bekerja, persisten di NVS.
- Tahap berikut: W2 (Guards adaptif) → W3 (Auto‑Trigger AC aktif benar) → W4 (OTA Upload).
