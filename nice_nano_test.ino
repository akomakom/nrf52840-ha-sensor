// =============================================================
//  Nice!Nano (nRF52840) Test Sketch
//  Board: Nice!Nano  |  BSP: nRF52 Arduino
//
//  Three independent proofs of life:
//    1. USB Serial  -- open Serial Monitor @ 115200
//    2. LED_BUILTIN blink at 1 Hz
//    3. Pin 2 square wave at 1 Hz -- measure with DMM vs GND
//       (expect ~1.65 V average, or watch it toggle if your
//        meter has a fast enough update rate)
// =============================================================

#define DMM_PIN 2   // easy-to-probe GPIO; change if needed

static bool     pinState  = false;
static uint32_t lastToggle = 0;
static uint32_t lastPrint  = 0;
static uint32_t tick       = 0;

void setup() {
  Serial.begin(115200);

  // Wait up to 4 s for USB CDC to enumerate (skip if no PC attached)
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 4000)) delay(10);

  Serial.println();
  Serial.println("====================================");
  Serial.println("  Nice!Nano test sketch  --  ALIVE");
  Serial.println("====================================");
  Serial.print  ("  LED_BUILTIN = pin "); Serial.println(LED_BUILTIN);
  Serial.print  ("  DMM_PIN     = pin "); Serial.println(DMM_PIN);
  Serial.println("  Both toggle at 1 Hz");
  Serial.println("====================================");
  Serial.println();

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(DMM_PIN,     OUTPUT);
}

void loop() {
  uint32_t now = millis();

  // --- Toggle LED + DMM pin every 500 ms (= 1 Hz square wave) ---
  if (now - lastToggle >= 500) {
    lastToggle = now;
    pinState   = !pinState;
    digitalWrite(LED_BUILTIN, pinState);
    digitalWrite(DMM_PIN,     pinState);
  }

  // --- Serial heartbeat every second ---
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    tick++;
    Serial.print("tick ");
    Serial.print(tick);
    Serial.print("  |  uptime ");
    Serial.print(now / 1000);
    Serial.print("s  |  pin=");
    Serial.println(pinState ? "HIGH (3.3 V)" : "LOW  (0 V)");
  }
}
