/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  USS Enterprise HomeKit Controller                           ║
 * ║  Tomy Enterprise Refit → ESP32-S3 → HomeKit via HomeSpan    ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * WIRING
 * ──────
 *   GPIO 4  →  220Ω  →  Pad A  (Mode Select)
 *   GPIO 5  →  220Ω  →  Pad B  (Fire Control)
 *   GND     →          Pad G  (shared ground)
 *
 * TOUCH SIMULATION PRINCIPLE
 * ──────────────────────────
 * Capacitive touch ICs detect increased capacitance to GND on their
 * electrode. Briefly pulling the electrode LOW through 220Ω mimics
 * the conductance path a finger provides.
 *
 * Idle state  = INPUT (high-Z) — don't disturb the touch circuit
 * Touch pulse = OUTPUT LOW for ~80ms, then release back to INPUT
 *
 * If touches are not registering reliably:
 *   Add a 47-100 pF capacitor between the 220Ω and each touch pad.
 *   Then flip the logic:
 *     - Idle  = OUTPUT LOW (cap uncharged, no effect)
 *     - Touch = OUTPUT HIGH briefly (cap charges → injects charge
 *               into electrode, cleanly mimicking finger capacitance)
 *   Uncomment CAP_MODE below to switch to that approach.
 *
 * HOMEKIT ACCESSORIES (appear as two switches in the Home app)
 * ─────────────────────────────────────────────────────────────
 *   "USS Enterprise"    ON  → tap A once, wait 5 s, tap A 3 more times
 *                             (turns on → cycles through to Warp Speed)
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
 *      → choose "Enterprise Bridge" → enter code:  466-37-726
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

// ── Uncomment to use capacitor-injection mode ──────────────────────────────
// #define CAP_MODE

// ── Timing (ms) ───────────────────────────────────────────────────────────
static constexpr int TOUCH_PULSE_MS    =   80;   // Duration of a single press
static constexpr int TOUCH_SETTLE_MS   =   30;   // Settle time after release
static constexpr int PRESS_GAP_MS      = 1000;   // Gap between repeated presses
static constexpr int POWER_OFF_HOLD_MS = 3300;   // Hold duration for OFF
static constexpr int STARTUP_WAIT_MS   = 5000;   // Wait for startup animation

// ── Touch simulation primitives ────────────────────────────────────────────

void pinIdle(int pin) {
#ifdef CAP_MODE
    // With capacitor: idle = LOW (cap uncharged = no injection)
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
#else
    // Direct mode: idle = high-impedance (don't load the touch circuit)
    pinMode(pin, INPUT);
#endif
}

void touchPress(int pin) {
#ifdef CAP_MODE
    // With capacitor: pulse HIGH → cap charges → injects charge into electrode
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    delay(TOUCH_PULSE_MS);
    pinIdle(pin);
#else
    // Direct mode: briefly pull LOW → adds conductance path to GND
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delay(TOUCH_PULSE_MS);
    pinIdle(pin);
#endif
    delay(TOUCH_SETTLE_MS);
}

// N presses, separated by gapMs
void touchMulti(int pin, int count, int gapMs = PRESS_GAP_MS) {
    for (int i = 0; i < count; i++) {
        touchPress(pin);
        if (i < count - 1) delay(gapMs);
    }
}

// Hold press for durationMs (used for power-off)
void touchHold(int pin, int durationMs) {
#ifdef CAP_MODE
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
#else
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
#endif
    delay(durationMs);
    pinIdle(pin);
}

// ── Service: Enterprise power switch ──────────────────────────────────────
//   ON  → tap A (1×), wait 5 s, tap A (3×) = 4 total → Warp Speed Mode
//   OFF → hold A 3.3 s → power-down sequence

struct EnterpriseSwitch : Service::Switch {
    SpanCharacteristic *power;

    EnterpriseSwitch() : Service::Switch() {
        power = new Characteristic::On(false);
    }

    boolean update() override {
        if (power->getNewVal()) {
            LOG1("Enterprise: Power ON → cycling to Warp Speed Mode\n");

            touchPress(PIN_A);                  // Tap 1 — lights on
            delay(STARTUP_WAIT_MS);             // Wait for startup animation
            touchMulti(PIN_A, 4, PRESS_GAP_MS);// Taps 2-5 — Underway → Impulse → Full → Warp

        } else {
            LOG1("Enterprise: Power OFF → sending hold sequence\n");

            touchHold(PIN_A, POWER_OFF_HOLD_MS);
        }

        return true;
    }
};

// ── Service: Weapons / Battle Mode (auto-resets to OFF) ────────────────────
//   ON → triple-tap B (1 s apart) → Battle Mode engages
//   Auto-resets so the switch can be triggered again without toggling off first

struct WeaponsSwitch : Service::Switch {
    SpanCharacteristic *power;

    WeaponsSwitch() : Service::Switch() {
        power = new Characteristic::On(false);
    }

    boolean update() override {
        if (power->getNewVal()) {
            LOG1("Enterprise: FIRE — Battle Mode\n");

            touchMulti(PIN_B, 3, 1000);         // Triple-tap B, 1 s apart

            // Brief pause, then auto-reset to OFF in HomeKit
            delay(300);
            power->setVal(false);
        }
        // Ignore the OFF transition (it's internal / auto-reset)
        return true;
    }
};

// ── Setup ──────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // Start both pins in idle state — don't disturb the touch circuit on boot
    pinIdle(PIN_A);
    pinIdle(PIN_B);

    homeSpan.setLogLevel(1);

    // "Enterprise-Bridge" is the name shown during HomeKit pairing
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
