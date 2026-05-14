/*
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  USS Enterprise NCC-1701 — Controller with I2S Speaker Output        ║
 * ║  HomeKit + Web Interface + Home Assistant (MQTT) + MAX98357A audio   ║
 * ║  Tomy Enterprise Refit → ESP32-S3                                    ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * TOUCH BUTTON WIRING
 * ───────────────────
 *   GPIO 4 → 220Ω → PN2222 Base → Collector to Electrode A (Mode Select)
 *   GPIO 5 → 220Ω → PN2222 Base → Collector to Electrode B (Fire Control)
 *   Both PN2222 Emitters → GND (Pad G on sub-board)
 *
 *   Connect to COPPER ELECTRODE PLATE on back of each sub-board.
 *   GPIO HIGH = transistor ON = electrode to GND = touch registered.
 *
 * I2S AUDIO WIRING — MAX98357A
 * ─────────────────────────────
 *   ESP32 GPIO 6  →  MAX98357A BCLK
 *   ESP32 GPIO 7  →  MAX98357A LRC
 *   ESP32 GPIO 8  →  MAX98357A DIN
 *   ESP32 3V3     →  MAX98357A Vin  (also tie SD to 3V3 — always enabled)
 *   ESP32 3V3     →  MAX98357A SD
 *   ESP32 GND     →  MAX98357A GND
 *   MAX98357A +/− →  Speaker (4–8Ω, max 3W)
 *   GAIN pin      →  leave floating (9dB default)
 *
 * AUDIO MODES (settable from Web UI, persisted in NVS)
 * ─────────────────────────────────────────────────────
 *   Browser mode: sounds play on the browser/phone running the Web UI
 *                 (uses Web Audio API — sample-accurate on iOS/Chrome)
 *   Speaker mode: sounds play through the I2S amp/speaker in the base
 *                 Works with HomeKit and HA automations too (not just Web UI)
 *
 * INTERFACES
 * ──────────
 *   HomeKit  : pair via Home app, code 836-17-294
 *   Web UI   : http://<ESP32_IP>:8080
 *   Home Asst: auto-discovered via MQTT (set MQTT_BROKER below)
 *
 * HOMEKIT ACCESSORIES (all momentary — no state confusion)
 * ─────────────────────────────────────────────────────────
 *   Power ON         momentary → tap A, 17s wait, 3× A (2s apart) → Warp
 *   Power OFF        momentary → hold A 5 s
 *   Power Mode       momentary → single press A
 *   Weapons          momentary → single press B (plays phaser in speaker mode)
 *   Fire 2 Torpedoes momentary → double tap B, 0.5 s apart
 *   Fire Everything  momentary → triple tap B, 0.5 s apart (Battle Mode)
 *   Anthem Startup   momentary → plays anthem + warp sequence (speaker mode)
 *
 * MQTT / HOME ASSISTANT
 * ──────────────────────
 *   Set MQTT_BROKER to enable. Leave empty "" to disable.
 *   Entities auto-appear in HA on first connection.
 *
 * FIRST-TIME SETUP
 * ────────────────
 *   1. Set MQTT_BROKER below (or leave "" to skip)
 *   2. Upload sketch (all 4 .h files must be in the same folder)
 *   3. Serial Monitor 115200 → type  W <SSID> <password>  → Enter
 *   4. Reboot — note IP address printed
 *   5. Home app → Add Accessory → More options → 836-17-294
 *   6. Web UI: http://<IP>:8080
 *
 * LIBRARIES
 * ─────────
 *   HomeSpan 2.x, PubSubClient — Arduino Library Manager
 *   ESP8266Audio — https://github.com/earlephilhower/ESP8266Audio
 *     (install via Library Manager: search "ESP8266Audio")
 *   Board: ESP32S3 Dev Module, USB CDC on Boot: Enabled
 */

// ── Network hostname (shown in router DHCP table) ─────────────────────────
#define DEVICE_HOSTNAME  "Tomy-Refit-Enterprise"

// ── MQTT Configuration ─────────────────────────────────────────────────────
#define MQTT_BROKER  ""       // ← Set to broker IP to enable, e.g. "192.168.1.10"
#define MQTT_PORT    1883
#define MQTT_USER    ""       // Leave empty if no auth required
#define MQTT_PASS    ""
#define MQTT_ID      "enterprise_ncc1701"

// ── Includes ───────────────────────────────────────────────────────────────
#include "HomeSpan.h"
#include <Preferences.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioOutputMixer.h>
#include "anthem_mp3.h"   // Anthem startup music
#include "torpedo_mp3.h"  // Torpedo launch sound (~841ms)
#include "phaser_mp3.h"   // Phaser fire sound (~763ms)

// ── Pins ───────────────────────────────────────────────────────────────────
static constexpr int PIN_A    = 4;
static constexpr int PIN_B    = 5;
static constexpr int I2S_BCLK = 6;
static constexpr int I2S_LRC  = 7;
static constexpr int I2S_DIN  = 8;

// ── Audio output mode ──────────────────────────────────────────────────────
// AUDIO_BROWSER: sounds play in the browser/phone (Web Audio API)
// AUDIO_SPEAKER: sounds play through the I2S amp/speaker in the base
#define AUDIO_BROWSER 0
#define AUDIO_SPEAKER 1
volatile int   audioMode  = AUDIO_BROWSER;
volatile float speakerVol = 0.25f;   // 0.0–1.0; start quiet so first power-on doesn't blast
// MAX98357A is already at its 9 dB hardware gain. Anything above ~0.5 of full
// software scale clips noticeably; keep this conservative.
#define I2S_MAX_GAIN  0.5f

// ── I2S audio task + dual-channel scheduled sound queue ────────────────────
// Two independent channels feed an AudioOutputMixer so phaser and torpedo
// sounds can play simultaneously without one blocking the other.
// Channel 0: torpedo + anthem   Channel 1: phaser
// scheduleSnd() routes each request to its channel queue and returns immediately.
// audioTask() services both channels every FreeRTOS tick.
struct SoundReq { const uint8_t* data; size_t len; uint32_t schedMs; };
#define SOUND_QUEUE_SIZE 10
QueueHandle_t  torpedoQueue = nullptr;
QueueHandle_t  phaserQueue  = nullptr;
AudioOutputI2S* i2sOut      = nullptr;

void scheduleSnd(const uint8_t* data, size_t len, uint32_t delayMs) {
    if (audioMode != AUDIO_SPEAKER) return;
    SoundReq r = { data, len, millis() + delayMs };
    QueueHandle_t q = (data == PHASER_MP3) ? phaserQueue : torpedoQueue;
    if (q) xQueueSend(q, &r, 0);
}

void audioTask(void*) {
    i2sOut = new AudioOutputI2S();
    i2sOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
    i2sOut->SetGain(speakerVol * I2S_MAX_GAIN);

    AudioOutputMixer*     mixer    = new AudioOutputMixer(32, i2sOut);
    AudioOutputMixerStub* stub[2]  = { mixer->NewInput(), mixer->NewInput() };
    QueueHandle_t*        queues[2]= { &torpedoQueue, &phaserQueue };

    struct Chan {
        bool     hasPending = false;
        SoundReq pending    = {};
        AudioFileSourcePROGMEM* src = nullptr;
        AudioGeneratorMP3*      mp3 = nullptr;
    } ch[2];

    for (;;) {
        uint32_t now = millis();

        for (int i = 0; i < 2; i++) {
            Chan& c = ch[i];

            // Advance active decoder
            if (c.mp3 && c.mp3->isRunning()) {
                if (!c.mp3->loop()) {
                    c.mp3->stop();
                    delete c.mp3; c.mp3 = nullptr;
                    delete c.src; c.src = nullptr;
                }
            }

            // Pull next request when channel is idle
            if (!c.hasPending && !c.mp3) {
                SoundReq r;
                if (xQueueReceive(*queues[i], &r, 0) == pdTRUE) {
                    c.pending    = r;
                    c.hasPending = true;
                }
            }

            // Start decoding when scheduled time arrives
            if (c.hasPending && (int32_t)(now - c.pending.schedMs) >= 0) {
                c.hasPending = false;
                // Re-apply gain so volume-slider changes take effect immediately
                i2sOut->SetGain(speakerVol * I2S_MAX_GAIN);
                stub[i]->SetGain(1.0f);
                c.src = new AudioFileSourcePROGMEM(c.pending.data, c.pending.len);
                c.mp3 = new AudioGeneratorMP3();
                c.mp3->begin(c.src, stub[i]);
            }
        }

        vTaskDelay(1);
    }
}

// ── Timing ─────────────────────────────────────────────────────────────────
static constexpr int PRESS_MS        =  200;   // Single press duration (HIGH)
static constexpr int SETTLE_MS       =  100;   // Settle after release
static constexpr int STARTUP_WAIT_MS = 17000;  // Wait after tap 1 for animation
static constexpr int WARP_GAP_MS     =  2000;  // Between mode-cycle presses
static constexpr int TORPEDO_GAP_MS  =   500;  // Between torpedo presses
static constexpr int POWEROFF_MS     =  5000;  // Hold A for power-down

// ── Anthem Startup timing (tweak independently from Power ON) ──────────────
// Adjust these to sync button presses with the music
static constexpr int ANTHEM_STARTUP_WAIT_MS = 17000;  // Wait after tap 1 (startup animation)
static constexpr int ANTHEM_WARP_GAP_MS     =  2000;  // Gap between presses 2 and 3
static constexpr int ANTHEM_LAST_GAP_MS     =  2000;  // Gap before final (4th) press — tweak to hit music cue

// ── Sound timing offsets (ms after button press) ──────────────────────────
// These control when audio plays relative to when the visual effect appears.
// Positive = audio plays AFTER button press, negative is not possible in JS
// (browser plays immediately on button tap; these delay the SECOND sound etc.)
//
// Button B (single press) — phaser alternates
static constexpr int SND_PHASER_DELAY_MS      =    0;  // ms after tap before phaser sound starts

// Fire 2 Torpedoes — double tap B, 0.5s apart
static constexpr int SND_TORP1_DELAY_MS       =    0;  // ms after tap 1 before torpedo 1 sound
static constexpr int SND_TORP2_DELAY_MS       =  500;  // ms after tap 1 before torpedo 2 sound (matches TORPEDO_GAP_MS)

// Fire Everything — triple tap B
// Sequence: tap1 → tap2 (0.5s) → tap3 (0.5s)
// Visuals: torpedo pulses, then phaser banks on saucer illuminate
static constexpr int SND_ALL_TORP1_DELAY_MS   =    0;  // torpedo 1
static constexpr int SND_ALL_TORP2_DELAY_MS   =  500;  // torpedo 2
static constexpr int SND_ALL_PHASER_DELAY_MS  = 1200;  // phasers engage after torpedoes

// ── MQTT ───────────────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

#define T_POWER_ON_CMD  "enterprise/power_on/set"
#define T_POWER_OFF_CMD "enterprise/power_off/set"
#define T_PRESS_A_CMD   "enterprise/press_a/set"
#define T_PRESS_B_CMD   "enterprise/press_b/set"
#define T_FIRE2_CMD     "enterprise/fire2/set"
#define T_FIRE3_CMD     "enterprise/fire3/set"
#define T_ANTHEM_CMD    "enterprise/anthem/set"

// ── Web UI (runs on Core 0 via FreeRTOS task) ──────────────────────────────
WebServer webUI(8080);
Preferences prefs;

// ── Runtime timing values (loaded from NVS, overridable via Web UI) ────────
struct Timing {
    // Button B
    int phaser_delay      =  1900;
    // Fire 2
    int f2_torp1          =  2900;
    int f2_torp2          =  3200;
    // Fire Everything
    int fe_p1             =  3000;
    int fe_t1             =  4700;
    int fe_t2             =  5100;
    int fe_p2             =  5800;
    int fe_p3             =  8700;
    int fe_t3             =  9900;
    int fe_t4             = 10700;
    int fe_p4             = 11100;
    // Anthem
    int anthem_wait       = 17000;
    int anthem_gap        =  2000;
    int anthem_last_gap   =  2000;
} T;

void loadTimings() {
    prefs.begin("timing", true);  // read-only
    T.phaser_delay    = prefs.getInt("phaser_delay",    T.phaser_delay);
    T.f2_torp1        = prefs.getInt("f2_torp1",        T.f2_torp1);
    T.f2_torp2        = prefs.getInt("f2_torp2",        T.f2_torp2);
    T.fe_p1           = prefs.getInt("fe_p1",           T.fe_p1);
    T.fe_t1           = prefs.getInt("fe_t1",           T.fe_t1);
    T.fe_t2           = prefs.getInt("fe_t2",           T.fe_t2);
    T.fe_p2           = prefs.getInt("fe_p2",           T.fe_p2);
    T.fe_p3           = prefs.getInt("fe_p3",           T.fe_p3);
    T.fe_t3           = prefs.getInt("fe_t3",           T.fe_t3);
    T.fe_t4           = prefs.getInt("fe_t4",           T.fe_t4);
    T.fe_p4           = prefs.getInt("fe_p4",           T.fe_p4);
    T.anthem_wait     = prefs.getInt("anthem_wait",     T.anthem_wait);
    T.anthem_gap      = prefs.getInt("anthem_gap",      T.anthem_gap);
    T.anthem_last_gap = prefs.getInt("anthem_last_gap", T.anthem_last_gap);
    audioMode         = prefs.getInt  ("audio_mode",   AUDIO_BROWSER);
    speakerVol        = prefs.getFloat("speaker_vol",  0.25f);
    prefs.end();
    Serial.println("Settings loaded from NVS");
}

void saveAudioSettings() {
    prefs.begin("timing", false);
    prefs.putInt  ("audio_mode",  audioMode);
    prefs.putFloat("speaker_vol", speakerVol);
    prefs.end();
}

void saveTimings() {
    prefs.begin("timing", false);  // read-write
    prefs.putInt("phaser_delay",    T.phaser_delay);
    prefs.putInt("f2_torp1",        T.f2_torp1);
    prefs.putInt("f2_torp2",        T.f2_torp2);
    prefs.putInt("fe_p1",           T.fe_p1);
    prefs.putInt("fe_t1",           T.fe_t1);
    prefs.putInt("fe_t2",           T.fe_t2);
    prefs.putInt("fe_p2",           T.fe_p2);
    prefs.putInt("fe_p3",           T.fe_p3);
    prefs.putInt("fe_t3",           T.fe_t3);
    prefs.putInt("fe_t4",           T.fe_t4);
    prefs.putInt("fe_p4",           T.fe_p4);
    prefs.putInt("anthem_wait",     T.anthem_wait);
    prefs.putInt("anthem_gap",      T.anthem_gap);
    prefs.putInt("anthem_last_gap", T.anthem_last_gap);
    prefs.end();
    Serial.println("Timings saved to NVS");
}

void resetTimings() {
    prefs.begin("timing", false);
    prefs.clear();
    prefs.end();
    T = Timing();  // reset to defaults
    Serial.println("Timings reset to defaults");
}

// ── Press primitives ───────────────────────────────────────────────────────

void simPress(int pin) {
    digitalWrite(pin, HIGH);
    delay(PRESS_MS);
    digitalWrite(pin, LOW);
    delay(SETTLE_MS);
}

void simMulti(int pin, int count, int gapMs) {
    for (int i = 0; i < count; i++) {
        simPress(pin);
        if (i < count - 1) delay(gapMs);
    }
}

void simHold(int pin, int ms) {
    digitalWrite(pin, HIGH);
    delay(ms);
    digitalWrite(pin, LOW);
    delay(SETTLE_MS);
}

// ── Actions — shared by HomeKit, Web UI, and MQTT ─────────────────────────

void actPowerOn() {
    LOG1("Action: Power ON → Warp Speed\n");
    simPress(PIN_A);
    delay(STARTUP_WAIT_MS);
    simMulti(PIN_A, 3, WARP_GAP_MS);
}

void actPowerOff() {
    LOG1("Action: Power OFF\n");
    simHold(PIN_A, POWEROFF_MS);
}

void actAnthemStartup() {
    LOG1("Action: Anthem Startup → Warp Speed\n");
    scheduleSnd(ANTHEM_MP3, ANTHEM_MP3_LEN, 0);   // plays through I2S in speaker mode
    // Same button sequence as Power ON to Warp — tweak ANTHEM_ constants to sync with music
    simPress(PIN_A);                              // Tap 1 — lights on
    delay(T.anthem_wait);                         // Wait for startup animation
    simPress(PIN_A); delay(T.anthem_gap);         // Tap 2 → Underway
    simPress(PIN_A); delay(T.anthem_last_gap);    // Tap 3 → Impulse/Full Power
    simPress(PIN_A);                              // Tap 4 → Warp Speed
}

void actPressA()  { LOG1("Action: Press A\n");            simPress(PIN_A); }

void actPressB() {
    LOG1("Action: Press B\n");
    scheduleSnd(PHASER_MP3, PHASER_MP3_LEN, T.phaser_delay);
    simPress(PIN_B);
}

void actFire2() {
    LOG1("Action: Fire 2 torpedoes\n");
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.f2_torp1);
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.f2_torp2);
    simMulti(PIN_B, 2, TORPEDO_GAP_MS);
}

void actFire3() {
    LOG1("Action: Fire everything\n");
    // P → T → T → P → P → T → (T + P simultaneous)
    scheduleSnd(PHASER_MP3,  PHASER_MP3_LEN,  T.fe_p1);
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.fe_t1);
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.fe_t2);
    scheduleSnd(PHASER_MP3,  PHASER_MP3_LEN,  T.fe_p2);
    scheduleSnd(PHASER_MP3,  PHASER_MP3_LEN,  T.fe_p3);
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.fe_t3);
    scheduleSnd(TORPEDO_MP3, TORPEDO_MP3_LEN, T.fe_t4);
    scheduleSnd(PHASER_MP3,  PHASER_MP3_LEN,  T.fe_p4);
    simMulti(PIN_B, 3, TORPEDO_GAP_MS);
}

// ── MQTT ───────────────────────────────────────────────────────────────────

bool mqttEnabled() { return strlen(MQTT_BROKER) > 0; }

void mqttPublishDiscovery() {
    String dev = "\"device\":{\"identifiers\":[\"enterprise_ncc1701\"],"
                 "\"name\":\"USS Enterprise NCC-1701\","
                 "\"model\":\"Tomy Enterprise Refit\","
                 "\"manufacturer\":\"Tomy\"}";

    auto btn = [&](const char* uid, const char* name, const char* topic) {
        String c = "{\"name\":\"" + String(name) + "\","
                   "\"unique_id\":\"" + String(uid) + "\","
                   "\"command_topic\":\"" + String(topic) + "\","
                   "\"payload_press\":\"PRESS\"," + dev + "}";
        mqtt.publish(("homeassistant/button/" + String(uid) + "/config").c_str(),
                     c.c_str(), true);
    };

    btn("enterprise_power_on",  "Power ON → Warp Speed", T_POWER_ON_CMD);
    btn("enterprise_power_off", "Power OFF",             T_POWER_OFF_CMD);
    btn("enterprise_press_a",   "Power Mode (Press A)",  T_PRESS_A_CMD);
    btn("enterprise_press_b",   "Weapons (Press B)",     T_PRESS_B_CMD);
    btn("enterprise_fire2",     "Fire 2 Torpedoes",      T_FIRE2_CMD);
    btn("enterprise_fire3",     "Fire Everything",       T_FIRE3_CMD);
    btn("enterprise_anthem",    "Anthem Startup",        T_ANTHEM_CMD);

    Serial.println("MQTT: HA discovery published");
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
    String t(topic);
    Serial.printf("MQTT rx: %s\n", topic);
    if      (t == T_POWER_ON_CMD)  actPowerOn();
    else if (t == T_POWER_OFF_CMD) actPowerOff();
    else if (t == T_PRESS_A_CMD)   actPressA();
    else if (t == T_PRESS_B_CMD)   actPressB();
    else if (t == T_FIRE2_CMD)     actFire2();
    else if (t == T_FIRE3_CMD)     actFire3();
    else if (t == T_ANTHEM_CMD)    actAnthemStartup();
}

void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (mqtt.connected()) return;
    Serial.print("MQTT: connecting...");
    bool ok = (strlen(MQTT_USER) > 0)
        ? mqtt.connect(MQTT_ID, MQTT_USER, MQTT_PASS)
        : mqtt.connect(MQTT_ID);
    if (ok) {
        Serial.println(" connected");
        mqtt.subscribe(T_POWER_ON_CMD);
        mqtt.subscribe(T_POWER_OFF_CMD);
        mqtt.subscribe(T_PRESS_A_CMD);
        mqtt.subscribe(T_PRESS_B_CMD);
        mqtt.subscribe(T_FIRE2_CMD);
        mqtt.subscribe(T_FIRE3_CMD);
        mqtt.subscribe(T_ANTHEM_CMD);
        mqttPublishDiscovery();
    } else {
        Serial.printf(" failed (rc=%d)\n", mqtt.state());
    }
}

static unsigned long lastMqttAttempt = 0;

void mqttLoop() {
    if (!mqttEnabled()) return;
    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - lastMqttAttempt > 10000) {
            lastMqttAttempt = now;
            mqttReconnect();
        }
    }
    mqtt.loop();
}

// ── HomeKit: Momentary switch ──────────────────────────────────────────────
// All HomeKit accessories are momentary — no persistent on/off state.
// This avoids HomeKit timeout issues with long sequences and state confusion
// since we cannot sense whether the Enterprise is actually on or off.

struct MomentarySwitch : Service::Switch {
    SpanCharacteristic *on;
    void (*action)();
    unsigned long resetAt = 0;

    MomentarySwitch(void (*fn)()) : Service::Switch(), action(fn) {
        on = new Characteristic::On(false);
    }
    boolean update() override {
        if (on->getNewVal()) {
            action();
            resetAt = millis() + 500;
        }
        return true;
    }
    void loop() override {
        if (resetAt > 0 && millis() >= resetAt) {
            on->setVal(false);
            resetAt = 0;
        }
    }
};

// ── Web UI HTML ────────────────────────────────────────────────────────────
const char HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>USS Enterprise</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;color:#eee;
     padding:16px;max-width:420px;margin:0 auto;min-height:100vh;
     background:url('data:image/jpeg;base64,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') center center/cover fixed;}
body::before{content:'';position:fixed;inset:0;background:rgba(0,0,10,0.72);z-index:0}
h1,p,.card,#st{position:relative;z-index:1}
h1{color:#aaddff;text-align:center;margin:14px 0 2px;font-size:22px;
    text-shadow:0 0 20px rgba(100,180,255,0.8)}
.sub{text-align:center;color:#88aacc;font-size:11px;margin-bottom:14px}
.card{background:rgba(8,8,30,0.82);border:1px solid rgba(80,120,200,0.3);
      border-radius:12px;padding:14px;margin-bottom:10px;
      backdrop-filter:blur(6px);position:relative;z-index:1}
h3{font-size:11px;color:#5599cc;text-transform:uppercase;letter-spacing:1px;margin-bottom:10px}
.note{font-size:11px;color:#6688aa;margin-bottom:8px;line-height:1.6}.note b{color:#88ccff}
button{display:block;width:100%;padding:14px;margin:5px 0;font-size:15px;
       font-weight:500;border:none;border-radius:9px;cursor:pointer;transition:opacity .15s}
button:active{opacity:.6}
.a  {background:rgba(20,80,40,0.9);color:#88ee99;border:1px solid #2a7c3a}
.b  {background:rgba(80,50,0,0.9);color:#ffcc44;border:1px solid #7c5a00}
.off{background:rgba(80,0,16,0.9);color:#ff7788;border:1px solid #880020}
.seq{background:rgba(20,20,80,0.9);color:#99aaff;border:1px solid #2a2a8c}
.anthem{background:rgba(60,20,80,0.9);color:#ddaaff;border:1px solid #7a4a9a}
.row{display:flex;gap:7px}.row button{flex:1;margin:0}
.tgroup{font-size:11px;color:#88aacc;margin:10px 0 4px;font-weight:600}
.trow{display:flex;align-items:center;justify-content:space-between;margin:3px 0}
.trow label{font-size:12px;color:#889;flex:1}
.trow input{width:80px;background:#0d0d20;border:1px solid #334;border-radius:5px;
            color:#aaddff;font-size:13px;padding:4px 6px;text-align:right}
.srow{display:flex;align-items:center;gap:10px;margin:6px 0}
.srow label{font-size:12px;color:#889;min-width:120px}
.srow input[type=range]{flex:1;accent-color:#88aacc}
.srow span{font-size:12px;color:#aaddff;min-width:36px;text-align:right}
.mode-btn{flex:1;padding:10px;font-size:13px;border:1px solid #334;border-radius:8px;
          background:#0d0d20;color:#889;cursor:pointer;transition:all .2s}
.mode-btn.active{background:rgba(20,60,100,0.9);color:#88ccff;border-color:#336699}
#st{text-align:center;min-height:26px;padding:5px;border-radius:7px;
    font-size:13px;margin:7px 0;position:relative;z-index:1}
#st.ok  {background:rgba(10,42,18,0.9);color:#44dd88}
#st.busy{background:rgba(26,26,10,0.9);color:#ddcc44}
#st.err {background:rgba(42,10,10,0.9);color:#dd4444}
.log{background:rgba(8,8,21,0.85);border:1px solid rgba(26,26,51,0.8);border-radius:6px;
     padding:8px;font-family:monospace;font-size:11px;color:#6688aa;max-height:72px;
     overflow-y:auto;margin-top:8px}
.bar{height:4px;background:rgba(26,26,42,0.8);border-radius:2px;margin-top:8px;overflow:hidden}
.fill{height:100%;width:0%;background:#44ee88;border-radius:2px;transition:width .2s}
</style></head><body>
<h1>🖖 USS Enterprise</h1>
<p class="sub">NCC-1701 · HomeKit · Home Assistant · Web</p>

<div class="card">
  <h3>Status</h3>
  <div id="st"></div>
  <div class="log" id="log">Ready. Awaiting orders.</div>
  <div class="bar"><div class="fill" id="bar"></div></div>
</div>

<div class="card">
  <h3>Single Presses</h3>
  <div class="row">
    <button class="a" onclick="go('/pa','Press A',400)">Press A</button>
    <button class="b" onclick="pressBSound()">Press B</button>
  </div>
</div>

<div class="card">
  <h3>Power</h3>
  <div class="note">
    <b>ON:</b> Press A → 17s → 3× A (2s apart) → Warp Speed<br>
    <b>OFF:</b> Hold A for 5 seconds
  </div>
  <button class="seq" onclick="go('/power_on','Power ON → Warp Speed (~25 s)',25000)">
    ⚡ Power ON → Warp Speed
  </button>
  <button class="anthem" onclick="anthemStart()">
    🎵 Anthem Startup → Warp Speed
  </button>
  <button class="off" onclick="go('/power_off','Power OFF — hold A 5 s',5500)">
    ■ Power OFF
  </button>
</div>
<!-- Sounds are loaded and played through the Web Audio API (see script
     below). HTML5 <audio> elements are unreliable on iOS for overlapping /
     scheduled playback. -->

<div class="card">
  <h3>Weapons</h3>
  <div class="note">
    <b>Fire 2:</b> Double tap B, 0.5s apart<br>
    <b>Fire Everything:</b> Triple tap B — Battle Mode
  </div>
  <button class="b" onclick="fire2Sound()">◎◎ Fire 2 Torpedoes</button>
  <button class="b" onclick="fireAllSound()">◎◎◎ Fire Everything</button>
</div>

<div class="card">
  <h3>🔊 Audio Settings</h3>
  <div class="tgroup">Output mode</div>
  <div class="row" style="margin-bottom:10px">
    <button class="mode-btn" id="btn-browser" onclick="setMode(0)">🌐 Browser</button>
    <button class="mode-btn" id="btn-speaker" onclick="setMode(1)">🔊 Speaker</button>
  </div>
  <div class="srow">
    <label>Speaker volume</label>
    <input type="range" id="vol-slider" min="0" max="100" value="25"
           oninput="volChange(this.value)">
    <span id="vol-label">25%</span>
  </div>
  <div class="note" id="mode-note" style="margin-top:6px"></div>
</div>

<script>
var logs=['Ready. Awaiting orders.'];
function log(m){logs.push(new Date().toLocaleTimeString()+' '+m);
  if(logs.length>30)logs.shift();
  var e=document.getElementById('log');e.textContent=logs.join('\n');e.scrollTop=9999;}
function st(m,c){var e=document.getElementById('st');e.textContent=m;e.className=c||'';}
function pulse(ms){var b=document.getElementById('bar');
  b.style.width='100%';setTimeout(function(){b.style.width='0%';},ms);}
// ─── Audio playback (Web Audio API) ────────────────────────────────────────
// iOS Safari severely restricts HTML5 <audio>: only the first few elements
// touched inside a user gesture are unlocked, and setTimeout-delayed play()
// calls drift / are dropped. The Web Audio API lets us unlock once (per
// AudioContext), keep MP3s decoded in memory, schedule playback with
// sample-accurate timing, and overlap as many sources as we want.
//
// curMode mirrors the ESP32's audioMode: 0=browser, 1=speaker. In speaker
// mode the ESP32 plays sounds through I2S, so we skip browser playback.
var audioCtx = null;
var audioBuffers = {};   // name -> decoded AudioBuffer
var audioRaw     = {};   // name -> ArrayBuffer waiting to be decoded
var SOUND_NAMES  = ['phaser', 'torpedo', 'anthem'];
var curMode      = 0;    // populated by loadTimings()

// Pre-fetch raw bytes immediately on page load (no AudioContext required).
SOUND_NAMES.forEach(function(name) {
  fetch('/' + name + '.mp3')
    .then(function(r){ return r.arrayBuffer(); })
    .then(function(b){ audioRaw[name] = b; })
    .catch(function(e){ log('Load ' + name + ': ' + e.message); });
});

// Must be called from a user gesture (click) to unlock audio on iOS.
function ensureAudio() {
  if (!audioCtx) {
    var Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return false;
    audioCtx = new Ctx();
  }
  if (audioCtx.state === 'suspended') audioCtx.resume();
  // Decode anything we've fetched but not yet decoded.
  SOUND_NAMES.forEach(function(name) {
    if (audioBuffers[name] || !audioRaw[name]) return;
    var raw = audioRaw[name];
    audioRaw[name] = null;
    audioCtx.decodeAudioData(raw)
      .then(function(d){ audioBuffers[name] = d; })
      .catch(function(e){ log('Decode ' + name + ': ' + e.message); });
  });
  return true;
}

// Play a sound after an optional delay (ms). The id may include a numeric
// suffix (e.g. 'phaser2', 'torpedo3') left over from the old per-element
// scheme — we map it back to the underlying sound name.
function playAt(id, delayMs) {
  if (curMode !== 0) return;   // speaker mode: ESP32 handles audio
  if (!ensureAudio()) return;
  var key = id.replace(/\d+$/, '');
  var when = audioCtx.currentTime + (delayMs || 0) / 1000;
  var fire = function() {
    var buf = audioBuffers[key];
    if (!buf) return false;
    var src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    src.start(Math.max(when, audioCtx.currentTime));
    return true;
  };
  if (fire()) return;
  // Buffer not yet decoded — poll briefly, then give up.
  var tries = 0;
  var poll = setInterval(function() {
    if (fire() || ++tries > 100) clearInterval(poll);
  }, 30);
}

function setMode(m) {
  curMode = m;
  updateModeUI(m);
  fetch('/save_audio', {method:'POST', headers:{'Content-Type':'application/json'},
    body:JSON.stringify({mode:m, vol:parseInt(document.getElementById('vol-slider').value)})
  }).then(function(){ log('Audio mode: ' + (m===0?'browser':'speaker')); });
}

function updateModeUI(m) {
  document.getElementById('btn-browser').classList.toggle('active', m===0);
  document.getElementById('btn-speaker').classList.toggle('active', m===1);
  document.getElementById('mode-note').textContent = m===0
    ? 'Audio plays on this browser/device.'
    : 'Audio plays on the speaker inside the Enterprise base. Works with HomeKit & HA too.';
}

function volChange(v) {
  document.getElementById('vol-label').textContent = v + '%';
  fetch('/save_audio', {method:'POST', headers:{'Content-Type':'application/json'},
    body:JSON.stringify({mode:curMode, vol:parseInt(v)})}).then(function(){});
}

// Timing values — defaults shown; overwritten by loadTimings() from ESP32 NVS
var T = {
  phaser_delay:1900,
  f2_torp1:2900,  f2_torp2:3200,
  fe_p1:3000,     fe_t1:4700,   fe_t2:5100,
  fe_p2:5800,     fe_p3:8700,
  fe_t3:9900,     fe_t4:10700,  fe_p4:11100,
  anthem_wait:17000, anthem_gap:2000, anthem_last_gap:2000
};

function loadTimings() {
  fetch('/timing').then(function(r){return r.json();}).then(function(d){
    // Separate audio fields from timing fields before overwriting T
    if (typeof d.audio_mode === 'number')  { curMode = d.audio_mode;  delete d.audio_mode; }
    var vol = typeof d.speaker_vol === 'number' ? d.speaker_vol : null;
    if (vol !== null) delete d.speaker_vol;
    T = d;
    Object.keys(T).forEach(function(k){
      var el = document.getElementById('t_'+k);
      if (el) el.value = T[k];
    });
    updateModeUI(curMode);
    if (vol !== null) {
      var pct = Math.round(vol * 100);
      document.getElementById('vol-slider').value = pct;
      document.getElementById('vol-label').textContent = pct + '%';
    }
  }).catch(function(){
    // If fetch fails (e.g. page loaded before WiFi), populate with defaults
    Object.keys(T).forEach(function(k){
      var el = document.getElementById('t_'+k);
      if (el) el.value = T[k];
    });
    updateModeUI(curMode);
  });
}

function pressBSound() {
  playAt('phaser', T.phaser_delay);   // single phaser bank
  go('/pb', 'Press B (Weapons)', 400);
}
function fire2Sound() {
  playAt('torpedo',  T.f2_torp1);   // torpedo 1
  playAt('torpedo2', T.f2_torp2);   // torpedo 2 (independent, can overlap)
  go('/fire2', 'Fire 2 Torpedoes', 1500);
}
function fireAllSound() {
  // P → T → T → P → P → T → (T + P) — phaser banks 4+5 fire visually together,
  // so we only play one phaser sound at the final beat.
  playAt('phaser',   T.fe_p1);   // [1] Phaser — single bank
  playAt('torpedo',  T.fe_t1);   // [2] Torpedo 1
  playAt('torpedo2', T.fe_t2);   // [3] Torpedo 2
  playAt('phaser2',  T.fe_p2);   // [4] Phaser bank 1
  playAt('phaser3',  T.fe_p3);   // [5] Phaser bank 2
  playAt('torpedo3', T.fe_t3);   // [6] Torpedo 3
  playAt('torpedo4', T.fe_t4);   // [7] Torpedo 4   ─┐ simultaneous
  playAt('phaser4',  T.fe_p4);   // [8] Phasers 4   ─┘
  go('/fire3', 'Fire Everything — Battle Mode', 3000);
}
function anthemStart() {
  playAt('anthem', 0);
  go('/anthem_on', 'Anthem Startup (~25 s)', T.anthem_wait + 7000);
}
function go(p,l,dur){st('⏳ '+l,'busy');log('→ '+l);pulse(dur||500);
  fetch(p).then(function(r){return r.text();}).then(function(t){
    st('✓ '+t,'ok');log('✓ '+t);
    setTimeout(function(){st('','');},5000);
  }).catch(function(){st('✗ Failed','err');log('✗ Failed');});}

function tpanel(){
  var p=document.getElementById('tpanel');
  p.style.display=p.style.display==='none'?'block':'none';}

function tchange(k,v){
  T[k]=parseInt(v)||0;}

function tsave(){
  fetch('/save_timing',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(T)}).then(function(r){return r.text();}).then(function(t){
    st('✓ Timings saved','ok');log('✓ Timings saved to device');
    setTimeout(function(){st('','');},3000);
  }).catch(function(){st('✗ Save failed','err');});}

function treset(){
  if(!confirm('Reset all timings to defaults?'))return;
  fetch('/reset_timing').then(function(){loadTimings();
    st('✓ Reset to defaults','ok');setTimeout(function(){st('','');},3000);});}

loadTimings();
</script>

<div class="card" style="margin-top:10px">
  <h3 onclick="tpanel()" style="cursor:pointer;user-select:none">
    ⚙ Timing Tweaker <span style="float:right;color:#446">▼ tap to expand</span>
  </h3>
  <div id="tpanel" style="display:none">
    <div class="note" style="margin-bottom:10px">All values in milliseconds from button tap. Changes apply immediately. Press Save to persist across reboots.</div>

    <div class="tgroup"><b>Button B — single phaser</b></div>
    <div class="trow"><label>Phaser delay</label><input type="number" id="t_phaser_delay" oninput="tchange('phaser_delay',this.value)"></div>

    <div class="tgroup"><b>Fire 2 Torpedoes</b></div>
    <div class="trow"><label>Torpedo 1</label><input type="number" id="t_f2_torp1" oninput="tchange('f2_torp1',this.value)"></div>
    <div class="trow"><label>Torpedo 2</label><input type="number" id="t_f2_torp2" oninput="tchange('f2_torp2',this.value)"></div>

    <div class="tgroup"><b>Fire Everything — P→T→T→P→P→T→(T+P)</b></div>
    <div class="trow"><label>[1] Phaser 1</label><input type="number" id="t_fe_p1" oninput="tchange('fe_p1',this.value)"></div>
    <div class="trow"><label>[2] Torpedo 1</label><input type="number" id="t_fe_t1" oninput="tchange('fe_t1',this.value)"></div>
    <div class="trow"><label>[3] Torpedo 2</label><input type="number" id="t_fe_t2" oninput="tchange('fe_t2',this.value)"></div>
    <div class="trow"><label>[4] Phaser 2</label><input type="number" id="t_fe_p2" oninput="tchange('fe_p2',this.value)"></div>
    <div class="trow"><label>[5] Phaser 3</label><input type="number" id="t_fe_p3" oninput="tchange('fe_p3',this.value)"></div>
    <div class="trow"><label>[6] Torpedo 3</label><input type="number" id="t_fe_t3" oninput="tchange('fe_t3',this.value)"></div>
    <div class="trow"><label>[7] Torpedo 4</label><input type="number" id="t_fe_t4" oninput="tchange('fe_t4',this.value)"></div>
    <div class="trow"><label>[8] Phasers 4</label><input type="number" id="t_fe_p4" oninput="tchange('fe_p4',this.value)"></div>

    <div class="tgroup"><b>Anthem Startup</b></div>
    <div class="trow"><label>Wait after tap 1 (ms)</label><input type="number" id="t_anthem_wait" oninput="tchange('anthem_wait',this.value)"></div>
    <div class="trow"><label>Gap tap 2→3 (ms)</label><input type="number" id="t_anthem_gap" oninput="tchange('anthem_gap',this.value)"></div>
    <div class="trow"><label>Gap tap 3→4 (ms)</label><input type="number" id="t_anthem_last_gap" oninput="tchange('anthem_last_gap',this.value)"></div>

    <div style="display:flex;gap:8px;margin-top:10px">
      <button class="seq" style="font-size:13px;padding:10px" onclick="tsave()">💾 Save to device</button>
      <button class="off" style="font-size:13px;padding:10px;flex:0 0 auto;width:auto" onclick="treset()">↺ Reset defaults</button>
    </div>
  </div>
</div>
</body></html>
)rawhtml";

// ── Web UI task (runs on Core 0, HomeSpan runs on Core 1) ─────────────────
void webTask(void*) {
    webUI.on("/", [](){
        webUI.send_P(200, "text/html", HTML);
    });
    webUI.on("/anthem.mp3", [](){
        webUI.sendHeader("Content-Type", "audio/mpeg");
        webUI.sendHeader("Cache-Control", "public, max-age=86400");
        webUI.send_P(200, "audio/mpeg", (const char*)ANTHEM_MP3, ANTHEM_MP3_LEN);
    });
    webUI.on("/torpedo.mp3", [](){
        webUI.sendHeader("Content-Type", "audio/mpeg");
        webUI.sendHeader("Cache-Control", "public, max-age=86400");
        webUI.send_P(200, "audio/mpeg", (const char*)TORPEDO_MP3, TORPEDO_MP3_LEN);
    });
    webUI.on("/phaser.mp3", [](){
        webUI.sendHeader("Content-Type", "audio/mpeg");
        webUI.sendHeader("Cache-Control", "public, max-age=86400");
        webUI.send_P(200, "audio/mpeg", (const char*)PHASER_MP3, PHASER_MP3_LEN);
    });
    webUI.on("/anthem_on", [](){ webUI.send(200,"text/plain","Anthem startup running..."); actAnthemStartup(); });

    // Timing API
    webUI.on("/timing", [](){
        String j = "{";
        j += "\"phaser_delay\":"    + String(T.phaser_delay)    + ",";
        j += "\"f2_torp1\":"        + String(T.f2_torp1)        + ",";
        j += "\"f2_torp2\":"        + String(T.f2_torp2)        + ",";
        j += "\"fe_p1\":"           + String(T.fe_p1)           + ",";
        j += "\"fe_t1\":"           + String(T.fe_t1)           + ",";
        j += "\"fe_t2\":"           + String(T.fe_t2)           + ",";
        j += "\"fe_p2\":"           + String(T.fe_p2)           + ",";
        j += "\"fe_p3\":"           + String(T.fe_p3)           + ",";
        j += "\"fe_t3\":"           + String(T.fe_t3)           + ",";
        j += "\"fe_t4\":"           + String(T.fe_t4)           + ",";
        j += "\"fe_p4\":"           + String(T.fe_p4)           + ",";
        j += "\"anthem_wait\":"     + String(T.anthem_wait)     + ",";
        j += "\"anthem_gap\":"      + String(T.anthem_gap)      + ",";
        j += "\"anthem_last_gap\":" + String(T.anthem_last_gap) + ",";
        j += "\"audio_mode\":"      + String(audioMode)         + ",";
        j += "\"speaker_vol\":"     + String(speakerVol, 3);
        j += "}";
        webUI.send(200, "application/json", j);
    });

    // Save audio mode + volume (separate POST so the timing-tweaker save
    // doesn't need to know about them).
    webUI.on("/save_audio", HTTP_POST, [](){
        if (!webUI.hasArg("plain")) { webUI.send(400, "text/plain", "No body"); return; }
        String body = webUI.arg("plain");
        auto getVal = [&](const char* key) -> int {
            String k = "\"" + String(key) + "\":";
            int idx = body.indexOf(k);
            if (idx < 0) return -1;
            idx += k.length();
            int end1 = body.indexOf(',', idx);
            int end2 = body.indexOf('}', idx);
            int end  = end1 < 0 ? end2 : (end2 < 0 ? end1 : min(end1, end2));
            return body.substring(idx, end).toInt();
        };
        int m = getVal("mode");
        if (m == AUDIO_BROWSER || m == AUDIO_SPEAKER) audioMode = m;
        int v = getVal("vol");   // 0–100 from slider
        if (v >= 0 && v <= 100) speakerVol = v / 100.0f;
        // Re-apply gain immediately so the next sound reflects the new volume
        // (audioTask also re-applies before each play, but this gives instant
        // feedback if a sound is currently mid-flight).
        if (i2sOut) i2sOut->SetGain(speakerVol * I2S_MAX_GAIN);
        saveAudioSettings();
        webUI.send(200, "text/plain", "Audio settings saved");
    });
    webUI.on("/save_timing", HTTP_POST, [](){
        if (webUI.hasArg("plain")) {
            String body = webUI.arg("plain");
            // Parse simple key=value pairs sent as JSON
            auto getVal = [&](const char* key) -> int {
                String k = "\"" + String(key) + "\":";
                int idx = body.indexOf(k);
                if (idx < 0) return -1;
                idx += k.length();
                return body.substring(idx, body.indexOf(',', idx) == -1 ?
                    body.indexOf('}', idx) : min(body.indexOf(',', idx), body.indexOf('}', idx))).toInt();
            };
            auto applyVal = [&](const char* key, int& field) {
                int v = getVal(key);
                if (v >= 0) field = v;
            };
            applyVal("phaser_delay",    T.phaser_delay);
            applyVal("f2_torp1",        T.f2_torp1);
            applyVal("f2_torp2",        T.f2_torp2);
            applyVal("fe_p1",           T.fe_p1);
            applyVal("fe_t1",           T.fe_t1);
            applyVal("fe_t2",           T.fe_t2);
            applyVal("fe_p2",           T.fe_p2);
            applyVal("fe_p3",           T.fe_p3);
            applyVal("fe_t3",           T.fe_t3);
            applyVal("fe_t4",           T.fe_t4);
            applyVal("fe_p4",           T.fe_p4);
            applyVal("anthem_wait",     T.anthem_wait);
            applyVal("anthem_gap",      T.anthem_gap);
            applyVal("anthem_last_gap", T.anthem_last_gap);
            saveTimings();
            webUI.send(200, "text/plain", "Saved");
        } else {
            webUI.send(400, "text/plain", "No body");
        }
    });
    webUI.on("/reset_timing", [](){
        resetTimings();
        webUI.send(200, "text/plain", "Reset to defaults");
    });
    webUI.on("/pa",        [](){ actPressA();  webUI.send(200,"text/plain","A pressed"); });
    webUI.on("/pb",        [](){ actPressB();  webUI.send(200,"text/plain","B pressed"); });
    webUI.on("/power_on",  [](){ webUI.send(200,"text/plain","Power ON sequence running..."); actPowerOn(); });
    webUI.on("/power_off", [](){ webUI.send(200,"text/plain","Power OFF complete"); actPowerOff(); });
    webUI.on("/fire2",     [](){ actFire2();   webUI.send(200,"text/plain","2 torpedoes fired"); });
    webUI.on("/fire3",     [](){ actFire3();   webUI.send(200,"text/plain","Battle Mode — all weapons firing"); });
    webUI.begin();
    Serial.println("Web UI started on port 8080");
    for (;;) {
        webUI.handleClient();
        delay(2);
    }
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    // Set pins LOW FIRST — prevents touch IC seeing boot transients
    pinMode(PIN_A, OUTPUT); digitalWrite(PIN_A, LOW);
    pinMode(PIN_B, OUTPUT); digitalWrite(PIN_B, LOW);
    delay(1000);

    loadTimings();  // Load saved timing + audio settings from NVS

    Serial.begin(115200);
    delay(200);

    // I2S audio task — created before HomeSpan so the first action (e.g. an
    // Anthem trigger right after reboot) finds the queue ready.
    torpedoQueue = xQueueCreate(SOUND_QUEUE_SIZE, sizeof(SoundReq));
    phaserQueue  = xQueueCreate(SOUND_QUEUE_SIZE, sizeof(SoundReq));
    xTaskCreatePinnedToCore(audioTask, "Audio", 16384, NULL, 2, NULL, 1);

    Serial.println("\n╔════════════════════════════════════════════════╗");
    Serial.println(  "║   USS Enterprise NCC-1701 — Speaker Edition    ║");
    Serial.println(  "╠════════════════════════════════════════════════╣");
    Serial.println(  "║  HomeKit  →  836-17-294                         ║");
    Serial.println(  "║  Web UI   →  http://<IP>:8080 (shown on connect)║");
    Serial.printf(   "║  Audio    →  %-34s║\n",
                     audioMode == AUDIO_SPEAKER ? "SPEAKER (I2S)" : "BROWSER");
    if (mqttEnabled()) {
        Serial.printf("║  MQTT     →  %-34s║\n", MQTT_BROKER);
    } else {
        Serial.println("║  MQTT     →  disabled (set MQTT_BROKER to enable)║");
    }
    Serial.println(  "╚════════════════════════════════════════════════╝\n");

    // MQTT init (only if configured)
    if (mqttEnabled()) {
        mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        mqtt.setKeepAlive(60);
        mqtt.setCallback(mqttCallback);
    }

    // HomeSpan
    homeSpan.setLogLevel(1);
    homeSpan.setPairingCode("83617294");  // HomeKit code: 836-17-294
    homeSpan.setHostNameSuffix("-" DEVICE_HOSTNAME);
    homeSpan.begin(Category::Bridges, "Enterprise-Bridge");

    // Power ON (momentary)
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Power ON to Warp");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-ON");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("2.0");
            new Characteristic::Identify();
        new MomentarySwitch(actPowerOn);

    // Power OFF (momentary)
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Power OFF Enterprise");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-OFF");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("2.0");
            new Characteristic::Identify();
        new MomentarySwitch(actPowerOff);

    // Power Mode — single press A
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Enterprise Mode (A)");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-A");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("2.0");
            new Characteristic::Identify();
        new MomentarySwitch(actPressA);

    // Weapons — single press B
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Enterprise Weapons (B)");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-B");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("2.0");
            new Characteristic::Identify();
        new MomentarySwitch(actPressB);

    // Fire 2 Torpedoes
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Fire 2 Torpedoes");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-F2");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("2.0");
            new Characteristic::Identify();
        new MomentarySwitch(actFire2);

    // Fire Everything
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Fire Everything");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-F3");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("3.0");
            new Characteristic::Identify();
        new MomentarySwitch(actFire3);

    // Anthem Startup — plays anthem MP3 (in speaker mode) plus the warp sequence
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Anthem Startup");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-ANTHEM");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("3.0");
            new Characteristic::Identify();
        new MomentarySwitch(actAnthemStartup);

    // Start web UI on Core 0 (HomeSpan/loop runs on Core 1)
    // Wait for WiFi first (HomeSpan connects WiFi during poll)
    Serial.println("Waiting for WiFi before starting Web UI...");
    while (WiFi.status() != WL_CONNECTED) {
        homeSpan.poll();
        delay(10);
    }
    String ip = WiFi.localIP().toString();
    Serial.println("\n╔════════════════════════════════════════════════╗");
    Serial.println(  "║           ★  ENTERPRISE ONLINE  ★              ║");
    Serial.println(  "╠════════════════════════════════════════════════╣");
    Serial.printf(   "║  Web UI   →  http://%-26s║\n", (ip + ":8080").c_str());
    Serial.println(  "║  HomeKit  →  836-17-294                         ║");
    if (mqttEnabled()) {
        Serial.printf("║  MQTT     →  %-34s║\n", MQTT_BROKER);
    } else {
        Serial.println("║  MQTT     →  disabled                           ║");
    }
    Serial.println(  "╚════════════════════════════════════════════════╝\n");

    xTaskCreatePinnedToCore(webTask, "WebUI", 8192, NULL, 1, NULL, 0);
}

// ── Loop (Core 1) ──────────────────────────────────────────────────────────
void loop() {
    homeSpan.poll();
    mqttLoop();
}
