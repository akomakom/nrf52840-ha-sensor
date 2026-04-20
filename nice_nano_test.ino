// =============================================================
//  I2C Pin Finder  (improved)
//  VCC=pin9, GND=pin10 fixed.
//  Skips crystal/reserved pins. After address ACK, attempts a
//  full SHT40 measurement + CRC to eliminate false positives.
// =============================================================

#define VCC_PIN    9
#define GND_PIN   10
#define SHT40_ADDR 0x44

// Pins to never drive: VCC, GND, USB D+/D-, crystal (P0.00/P0.01 = pins 0/1)
const uint8_t SKIP[] = { VCC_PIN, GND_PIN, 18,
                          0, 1,    // XL1/XL2 — 32 kHz crystal, SoftDevice owns them
                          17, 20   // QSPI / NFC on many variants
                        };

bool shouldSkip(uint8_t p) {
  if (p >= 48) return true;
  for (uint8_t s : SKIP) if (p == s) return true;
  return false;
}

// ---- Bit-bang I2C -------------------------------------------
uint8_t g_sda, g_scl;

static void sdaH() { pinMode(g_sda, INPUT_PULLUP); }
static void sdaL() { pinMode(g_sda, OUTPUT); digitalWrite(g_sda, LOW); }
static void sclH() { pinMode(g_scl, INPUT_PULLUP); }
static void sclL() { pinMode(g_scl, OUTPUT); digitalWrite(g_scl, LOW); }
static bool sdaR() { pinMode(g_sda, INPUT_PULLUP); return digitalRead(g_sda); }
static void d()    { delayMicroseconds(5); }

static void bbStart() { sdaH();d(); sclH();d(); sdaL();d(); sclL();d(); }
static void bbStop()  { sdaL();d(); sclH();d(); sdaH();d(); }

static bool bbWrite(uint8_t byte) {
  for (int i = 7; i >= 0; i--) {
    if ((byte >> i) & 1) sdaH(); else sdaL();
    d(); sclH(); d(); sclL(); d();
  }
  sdaH(); d(); sclH(); d();
  bool ack = !sdaR();
  sclL(); d();
  return ack;
}

static uint8_t bbRead(bool ack) {
  uint8_t val = 0;
  sdaH();
  for (int i = 7; i >= 0; i--) {
    d(); sclH(); d();
    if (sdaR()) val |= (1 << i);
    sclL();
  }
  if (ack) sdaL(); else sdaH();
  d(); sclH(); d(); sclL(); d();
  sdaH();
  return val;
}

static uint8_t crc8(uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

static void releasePins(uint8_t sda, uint8_t scl) {
  pinMode(sda, INPUT); pinMode(scl, INPUT);
}

// Returns true if SHT40 ACKs the address
static bool probeAddr(uint8_t sda, uint8_t scl) {
  g_sda = sda; g_scl = scl;
  bbStart();
  bool ack = bbWrite(SHT40_ADDR << 1);
  bbStop();
  releasePins(sda, scl);
  return ack;
}

// After address ACK: send measure command, read 6 bytes, check CRC
// Returns true only if we get valid temperature data
static bool verifyMeasurement(uint8_t sda, uint8_t scl) {
  g_sda = sda; g_scl = scl;

  // Send measure command 0xFD (high precision)
  bbStart();
  if (!bbWrite(SHT40_ADDR << 1)) { bbStop(); releasePins(sda, scl); return false; }
  if (!bbWrite(0xFD))            { bbStop(); releasePins(sda, scl); return false; }
  bbStop();

  delay(10);  // measurement time

  // Read 6 bytes
  bbStart();
  if (!bbWrite((SHT40_ADDR << 1) | 1)) { bbStop(); releasePins(sda, scl); return false; }
  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = bbRead(i < 5);
  bbStop();
  releasePins(sda, scl);

  // Check both CRCs
  if (crc8(buf, 2) != buf[2])   return false;
  if (crc8(buf+3, 2) != buf[5]) return false;

  // Sanity-check temperature: must be between -20 and +85 C
  uint16_t t_raw = ((uint16_t)buf[0] << 8) | buf[1];
  float tempC = -45.0f + 175.0f * (float)t_raw / 65535.0f;
  if (tempC < -20.0f || tempC > 85.0f) return false;

  return true;
}

// =============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(GND_PIN, OUTPUT); digitalWrite(GND_PIN, LOW);
  pinMode(VCC_PIN, OUTPUT); digitalWrite(VCC_PIN, HIGH);
  delay(10);

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 5000) delay(10);

  Serial.println("\n=========================================");
  Serial.println("  I2C Pin Finder  (SHT40 @ 0x44)");
  Serial.println("  VCC=pin9, GND=pin10");
  Serial.println("  Skipping: 0,1 (crystal), 9,10,18");
  Serial.println("  ACK candidate verified by full CRC read");
  Serial.println("=========================================\n");
  Serial.flush();
}

void loop() {
  bool found = false;
  uint8_t foundSDA = 0, foundSCL = 0;

  for (uint8_t sda = 0; sda < 48 && !found; sda++) {
    if (shouldSkip(sda)) continue;
    for (uint8_t scl = 0; scl < 48 && !found; scl++) {
      if (shouldSkip(scl) || scl == sda) continue;

      if (probeAddr(sda, scl)) {
        Serial.print("  ACK at SDA="); Serial.print(sda);
        Serial.print(" SCL="); Serial.print(scl);
        Serial.print(" — verifying...");
        Serial.flush();

        if (verifyMeasurement(sda, scl)) {
          Serial.println(" CONFIRMED!");
          foundSDA = sda; foundSCL = scl;
          found = true;
        } else {
          Serial.println(" false positive, skipping");
          Serial.flush();
        }
      }
    }

    if (!found && sda % 8 == 7) {
      Serial.print("  Tried SDA up to pin "); Serial.println(sda);
      Serial.flush();
    }
  }

  if (!found) {
    Serial.println("No SHT40 found in this pass. Retrying...\n");
    Serial.flush();
    delay(2000);
  } else {
    // Keep printing so monitor can open late
    while (true) {
      Serial.println("*** SHT40 CONFIRMED ***");
      Serial.print("  SDA = pin "); Serial.print(foundSDA);
      Serial.print("  (P"); Serial.print(foundSDA/32); Serial.print('.');
      if (foundSDA%32 < 10) Serial.print('0'); Serial.print(foundSDA%32); Serial.println(")");
      Serial.print("  SCL = pin "); Serial.print(foundSCL);
      Serial.print("  (P"); Serial.print(foundSCL/32); Serial.print('.');
      if (foundSCL%32 < 10) Serial.print('0'); Serial.print(foundSCL%32); Serial.println(")");
      Serial.flush();
      pinMode(LED_BUILTIN, OUTPUT);
      digitalWrite(LED_BUILTIN, HIGH); delay(500);
      digitalWrite(LED_BUILTIN, LOW);  delay(500);
    }
  }
}
