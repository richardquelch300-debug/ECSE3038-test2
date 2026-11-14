# ECSE3038 Test 2: The bathroom fan

An ESP32 in Wokwi reads humidity from a DHT22 and sends it to a FastAPI + MongoDB Atlas API. The API stores fans, readings and fan settings. When the settings change, it publishes them over MQTT, and the board switches its fan (an LED) to match.

- `api/`: the FastAPI app (`app.py`, `requirements.txt`, and a `.env` that is not committed)
- `board/`: the PlatformIO project (`src/main.cpp`, `diagram.json`, `wokwi.toml`, and an `include/secrets.h` that is not committed)

## MQTT topics (`<name>` = `MQTT_NAME`)

| Topic | Written by | Retained | Payload |
|---|---|---|---|
| `ecse3038/<name>/fan/settings` | API (`PUT /fans/{mac}/settings`) | yes | `{"mode": "auto", "max_humidity": 65.0}` |
| `ecse3038/<name>/fan/state` | board | yes | `on` / `off` |
| `ecse3038/<name>/status` | board, plus the broker via the last will | yes | `online` / `offline` |

## Running the API

1. Create `api/.env`:
   ```
   MONGODB_URI=mongodb+srv://<user>:<password>@<cluster>.mongodb.net/
   MQTT_NAME=<name>
   ```
2. On the VM:
   ```bash
   sudo systemctl stop iot-api
   git clone https://github.com/<you>/ECSE3038-test2.git
   cd ECSE3038-test2/api
   python3 -m venv venv
   source venv/bin/activate
   python -m pip install -r requirements.txt
   nano .env
   uvicorn app:app --host 127.0.0.1 --port 8000
   ```
   nginx forwards port 80 to 8000. The docs are at `http://<vm>/docs`. When finished: Ctrl+C, then `sudo systemctl start iot-api`.

### Routes

| Route | Notes |
|---|---|
| `POST /fans` | `{"mac", "room"}` → 201 with a generated `key`; a duplicate MAC → 409 |
| `GET /fans`, `GET /fans/{mac}` | keys are never shown; an unknown MAC → 404 |
| `POST /readings` | `{"mac", "humidity"}` + `X-API-Key`; 401 / 403 / 422 / 201 |
| `GET /readings` | newest first; `?mac=`, `?limit=` (1–100, default 10) |
| `PUT /fans/{mac}/settings` | `{"mode": auto\|on\|off, "max_humidity": 0–100}`; 201 for new settings, 200 when replacing; publishes retained on MQTT |
| `DELETE /fans/{mac}` | needs that fan's own key; 204 / 401 / 403 / 404 |

## Running the board

1. Open the `board` folder in VS Code (PlatformIO + Wokwi extensions).
2. Make `include/secrets.h`:
   ```c
   #pragma once
   #define DEVICE_KEY ""
   ```
3. At the top of `src/main.cpp`, check `API_URL` (your VM) and `MQTT_NAME` (the same name as in `.env`).
4. Build (PlatformIO: Build), then run with **Wokwi: Start Simulator**.
5. First run: copy the MAC from the Serial Monitor (a 401 is expected). `POST /fans` with that MAC, put the returned key in `secrets.h`, then build and start again.

The board posts humidity every 10 s. It connects to the class broker with an `offline` last will, publishes `online`, and listens on `fan/settings`. Messages that aren't JSON, are missing a field or have a bad mode are refused. Mode `on`/`off` forces the fan; `auto` turns it on when humidity > `max_humidity`. The fan stays off until the first settings arrive, and `fan/state` is published only when the fan changes.

Watch the topics at http://135.232.203.144/mqtt/ by subscribing to `ecse3038/<name>/#`.
