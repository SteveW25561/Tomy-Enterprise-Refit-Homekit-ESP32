/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  USS Enterprise HomeKit Controller                           ║
 * ║  Tomy Enterprise Refit → ESP32-S3 → HomeKit via HomeSpan    ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * WIRING
 * ──────
 *   GPIO 4  →  220Ω  →  [47-100pF cap]  →  Pad A  (Mode Select)
 *   GPIO 5  →  220Ω  →  [47-100pF cap]  →  Pad B  (Fire Control)
 *   GND     →                               Pad G  (shared ground)
 *
 *   The capacitor is required. Place it in series between the 220Ω
 *   resistor and the touch pad. A 47pF or 100pF ceramic cap works well.
 *
 * TOUCH SIMULATION — capacitor charge injection
 * ─────────────────────────────────────────────
 * The board already has 1kΩ series resistors (R22/R10) before the
 * touch IC, making direct LOW-pull insufficient. Charge injection used:
 *
 *   Idle  = OUTPUT LOW  (cap uncharged — no effect on electrode)
 *   Touch = OUTPUT HIGH briefly (cap charges → injects charge pulse
 *           into electrode, mimicking finger capacitance)
 *   Release = OUTPUT LOW again (cap discharges — touch ends)
 *
 * HOMEKIT ACCESSORIES (appear as two switches in the Home app)
 * ─────────────────────────────────────────────────────────────
 *   "USS Enterprise"    ON  → tap A once, wait 5 s, tap A 4 more times
 *                             (turns on → cycles to Warp Speed Mode)
 *                       OFF → hold A for 3.3 s (power-down sequence)
 *
 *   "Photon Torpedoes"  ON  → triple-tap B, 1 s apart (Battle Mode)
 *                             (auto-resets to OFF so it can be re-fired)
 *
 * FIRST-TIME SETUP (after flashing)
 * ──────────────────────────────────
 *   1. Open Serial Monitor at 115200 baud
 *   2. Type  W <SSID> <password>  then press Enter
 *   3. Reboot; the ESP32 joins WiFi and announces itself
 *   4. Open iPhone Home app → Add Accessory → "More options"
 *      → choose "Enterprise Bridge" → enter code:  473-92-615
 *   5. Accept both accessories when prompted
 *
 * LIBRARY
 * ───────
 *   HomeSpan 2.x — install via Arduino Library Manager
 *   Board: "ESP32S3 Dev Module"
 *   USB CDC on Boot: Enabled (if using the USB-OTG port for serial)
 */

#include "HomeSpan.h"

// ── Pin assignments ────────────────────────────────────────────────────────
static constexpr int PIN_A = 4;   // Mode Select
static constexpr int PIN_B = 5;   // Fire Control

// ── Timing (ms) ───────────────────────────────────────────────────────────
static constexpr int TOUCH_PULSE_MS    =  500;   // Long pulse to compensate for board series resistance   // Charge injection duration
static constexpr int TOUCH_SETTLE_MS   =   50;   // Settle time after release
static constexpr int PRESS_GAP_MS      = 1000;   // Gap between repeated presses
static constexpr int POWER_OFF_HOLD_MS = 3300;   // Hold duration for OFF
static constexpr int STARTUP_WAIT_MS   = 5000;   // Wait for startup animation

// ── Touch simulation — capacitor charge injection ──────────────────────────

// Idle: high-Z so we don't continuously load the touch circuit
void pinIdle(int pin) {
    pinMode(pin, INPUT);
}

// Touch: pull LOW → conductance path to GND mimics finger
// Longer pulse compensates for board's 1kΩ series resistors (R22/R10)
void touchPress(int pin) {
    digitalWrite(pin, LOW);           // Pull electrode toward GND
    delay(TOUCH_PULSE_MS);
    pinMode(pin, INPUT);              // Release to high-Z
    delay(TOUCH_SETTLE_MS);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);           // Return to idle LOW
}

// N presses, separated by gapMs
void touchMulti(int pin, int count, int gapMs = PRESS_GAP_MS) {
    for (int i = 0; i < count; i++) {
        touchPress(pin);
        if (i < count - 1) delay(gapMs);
    }
}

// Hold: pull LOW for durationMs then release to high-Z
void touchHold(int pin, int durationMs) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delay(durationMs);
    pinIdle(pin);
}

// ── Service: Enterprise power switch ──────────────────────────────────────
//   ON  → tap A (1×), wait 5 s, tap A (4×) = 5 total → Warp Speed Mode
//   OFF → hold A 3.3 s → power-down sequence

struct EnterpriseSwitch : Service::Switch {
    SpanCharacteristic *power;

    EnterpriseSwitch() : Service::Switch() {
        power = new Characteristic::On(false);
    }

    boolean update() override {
        if (power->getNewVal()) {
            LOG1("Enterprise: Power ON → cycling to Warp Speed Mode\n");
            touchPress(PIN_A);                   // Tap 1 — lights on
            delay(STARTUP_WAIT_MS);              // Wait for startup animation
            touchMulti(PIN_A, 4, PRESS_GAP_MS); // Taps 2-5 → Warp Speed
        } else {
            LOG1("Enterprise: Power OFF → sending hold sequence\n");
            touchHold(PIN_A, POWER_OFF_HOLD_MS);
        }
        return true;
    }
};

// ── Service: Weapons / Battle Mode ────────────────────────────────────────
//   ON → triple-tap B (1 s apart) → Battle Mode engages
//   Auto-resets to OFF via loop() to avoid HomeSpan update() conflict

struct WeaponsSwitch : Service::Switch {
    SpanCharacteristic *power;
    unsigned long resetAt = 0;        // millis() timestamp to reset switch

    WeaponsSwitch() : Service::Switch() {
        power = new Characteristic::On(false);
    }

    boolean update() override {
        if (power->getNewVal()) {
            LOG1("Enterprise: FIRE — Battle Mode\n");
            touchMulti(PIN_B, 3, 1000);          // Triple-tap B, 1 s apart
            resetAt = millis() + 300;            // Schedule reset in 300 ms
        }
        return true;
    }

    // loop() runs every HomeSpan poll cycle — safe place to set characteristic
    void loop() override {
        if (resetAt > 0 && millis() >= resetAt) {
            power->setVal(false);                // Reset switch to OFF
            resetAt = 0;
        }
    }
};

// ── Setup ──────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // Initialise pins to high-Z at boot — don't load touch circuit
    pinIdle(PIN_A);
    pinIdle(PIN_B);

    homeSpan.setLogLevel(1);
    homeSpan.setPairingCode("47392615");   // HomeKit pairing code: 473-92-615

    homeSpan.begin(Category::Bridges, "Enterprise-Bridge");

    // ── Accessory 1: Power ──────────────────────────────────────────
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("USS Enterprise");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("1.0");
            new Characteristic::Identify();
        new EnterpriseSwitch();

    // ── Accessory 2: Weapons ────────────────────────────────────────
    new SpanAccessory();
        new Service::AccessoryInformation();
            new Characteristic::Name("Photon Torpedoes");
            new Characteristic::Manufacturer("Tomy");
            new Characteristic::SerialNumber("NCC-1701-W");
            new Characteristic::Model("Enterprise Refit");
            new Characteristic::FirmwareRevision("1.0");
            new Characteristic::Identify();
        new WeaponsSwitch();
}

void loop() {
    homeSpan.poll();
}
