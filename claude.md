# USS Enterprise HomeKit Controller — Project Summary

## What This Is
A project to control the **Tomy Star Trek Enterprise Refit model** (tomyplus.tomy.com/startrek2024) via Apple HomeKit, a browser Web UI, and optionally Home Assistant — all running on an ESP32-S3 hidden inside the base.

## Hardware
- **ESP32-S3 Dev Board** (dual USB-C, 16MB flash) — [Amazon CA link](https://www.amazon.ca/dp/B0DB1WK3CW)
- **2× PN2222 NPN transistors** — one per touch button
- **2× 220Ω resistors** — base resistors for transistors
- **GPIO 4** → 220Ω → PN2222 Base → Collector to **copper electrode plate** (back of Button A sub-board)
- **GPIO 5** → 220Ω → PN2222 Base → Collector to **copper electrode plate** (back of Button B sub-board)
- Both Emitters → GND (Pad G on sub-board)
- Left USB-C port powers ESP32 (with USB CDC on Boot: Disabled)

## Critical Hardware Discovery
The Enterprise has two capacitive touch buttons (A = Mode Select, B = Fire Control). Each button is a small sub-board with:
- **Front:** S/G/V pads — S is a **digital OUTPUT** from the touch IC (0V idle, 3.3V pressed). Cannot be driven externally.
- **Back:** Large copper electrode plate — the **raw capacitive sensing element**. This is the correct connection point.

The transistor switches the electrode to GND briefly, mimicking a finger press. At idle, transistor is OFF so the touch IC calibrates to the natural electrode state. This was the key insight after extensive debugging.

## Repository
**GitHub:** https://github.com/SteveW25561/Tomy-Enterprise-Refit-Homekit-ESP32

**Files:**
- `enterprise_homekit.ino` — main sketch (HomeKit + Web UI + MQTT/HA)
- `enterprise_test.ino` — standalone test sketch (no HomeKit, web UI only)
- `anthem_mp3.h` — Star Trek theme MP3 as PROGMEM byte array (~290KB, mono 64kbps)
- `torpedo_mp3.h` — torpedo sound (~6.6KB)
- `phaser_mp3.h` — phaser sound (~6KB)
- `README.md` — full build instructions with photos
- `images/` — build photos

## Arduino IDE Settings
- Board: `ESP32S3 Dev Module`
- USB CDC on Boot: `Disabled` (left port = pure power; still flashable)
- Partition Scheme: `Huge App (3MB No OTA/1MB SPIFFS)`
- Upload Speed: `921600`

## Libraries Required
- **HomeSpan 2.x** — HomeKit implementation
- **PubSubClient** — MQTT for Home Assistant
- **Preferences** — built-in, NVS persistence for timing values

## Architecture
- **HomeSpan** runs on Core 1, port 80 — manages WiFi, HomeKit HAP
- **Web UI** runs on Core 0 via FreeRTOS task, port 8080 — served as embedded HTML with background image and audio files
- **MQTT** runs in main loop — optional, skipped if `MQTT_BROKER` left empty
- **Preferences/NVS** — persists timing values across reboots
- GPIO pins set LOW before anything else in `setup()` — prevents touch IC auto-calibrating to connected wire on boot

## HomeKit Accessories (all momentary switches)
1. Power ON to Warp
2. Power OFF Enterprise
3. Enterprise Mode (A) — single press A
4. Enterprise Weapons (B) — single press B
5. Fire 2 Torpedoes
6. Fire Everything

**Pairing code: 836-17-294**

## Button Sequences
| Action | Sequence |
|--------|----------|
| Power ON to Warp | Press A → wait 17s → Press A × 3 (2s apart) |
| Power OFF | Hold A for 5 seconds |
| Anthem Startup | Same as Power ON but plays Trek theme audio on browser |
| Press B (weapons) | Single tap B — alternates phaser banks |
| Fire 2 Torpedoes | Double tap B, 0.5s apart |
| Fire Everything | Triple tap B, 0.5s apart → P→T→T→P→P→T→(T+P+P) |

## Sound System
Audio files served by ESP32 at `/anthem.mp3`, `/torpedo.mp3`, `/phaser.mp3`. Browser plays them locally via Web Audio API. 4 independent `<audio>` elements per sound type allow overlapping playback. Timing controlled by JS variables loaded from ESP32 NVS via `/timing` endpoint.

**Fire Everything audio sequence:** P→T→T→P→P→T→(T+P+P simultaneous)
Uses `phaser`, `phaser2`, `phaser3`, `phaser4` and `torpedo`, `torpedo2`, `torpedo3`, `torpedo4` audio elements.

## Timing System
All sound delays stored in ESP32 NVS via `Preferences` library. Web UI has a collapsible **⚙ Timing Tweaker** panel at the bottom — changes apply immediately in browser, saved to NVS via POST to `/save_timing`, reset via `/reset_timing`.

**Current tuned defaults:**
```
phaser_delay: 1900    f2_torp1: 2900    f2_torp2: 3200
fe_p1: 3000    fe_t1: 4700    fe_t2: 5100
fe_p2: 5800    fe_p3: 8700    fe_t3: 9900
fe_t4: 10700   fe_p4: 11100   fe_p5: 11100
anthem_wait: 17000   anthem_gap: 2000   anthem_last_gap: 2000
```

## Transistor Wiring Note
Current build has collector/emitter reversed (emitter on electrode, collector to GND) — works in reverse active mode with slightly reduced gain. Does not affect reliability meaningfully and rewiring risks disturbing electrode connections.

## Future Plans
- 3D printed oval base cover (STL to be designed) for ESP32 mounting and WiFi signal clearance
- Optional I2S audio hat (MAX98357A + small speaker) to play sounds from Enterprise itself rather than browser
- Google Home via Home Assistant bridge (HA already integrated)