// =============================================================
//  BTHome v2 BLE Temperature / Humidity Sensor
//  nRF52840 (Nice!Nano clone)
//
//  SHT40 wiring:  SDA=pin24  SCL=pin22  VCC=pin17  GND=pin20
//  Protocol:      BTHome v2 — Home Assistant auto-discovery
//  Sleep:         WFE with UART disabled (~0.5-1 mA total)
//
//  Power optimizations:
//    - System OFF mode with RTC wakeup (device resets on wake)
//    - SoftDevice disabled during sleep
//    - UART disabled during sleep
//    - All unused peripherals disabled
//    - Reduced advertising time and interval
//    - Proper pin configuration for low leakage
//    - Sensor power gating between readings
//
//  Note: Device resets on wakeup, so setup() runs each cycle
//
//  Telemetry broadcast each cycle:
//    - Temperature (°C)
//    - Humidity (%)
//    - Supply voltage (V)  — measured via SAADC VDD channel
//    - Battery % estimate  — linear 2×AA: 3.2V=100%, 2.0V=0%
//                            reads ~100% on USB (VDD = 3.3V)
// =============================================================

#include <bluefruit.h>

// ---- Pin assignments ----------------------------------------
#define VCC_PIN    24
#define GND_PIN    22
#define SCL_PIN    20
#define SDA_PIN    17
#define SHT40_ADDR 0x44

// ---- Timing -------------------------------------------------
#define SLEEP_MS   300000UL   // 15 s (change to 300000UL for 5 min)
#define ADV_MS      2000     // advertise 2 s per cycle (sufficient for HA)

// ---- Diagnostic log (kept in RAM across sleep cycles) -------
#define LOG_SIZE 8
static float log_temp[LOG_SIZE];
static float log_rh[LOG_SIZE];
static uint8_t log_idx   = 0;
static uint32_t log_count = 0;

// =============================================================
// Supply voltage via SAADC VDD channel
// GAIN=1/6, REFSEL=Internal(0.6V), 12-bit → full scale = 3.6V
// No external pin or voltage divider required.
// =============================================================

static float readVDD() {
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;

  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit << SAADC_RESOLUTION_VAL_Pos;
  NRF_SAADC->OVERSAMPLE  = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;

  NRF_SAADC->CH[0].CONFIG =
      (SAADC_CH_CONFIG_GAIN_Gain1_6    << SAADC_CH_CONFIG_GAIN_Pos)   |
      (SAADC_CH_CONFIG_MODE_SE         << SAADC_CH_CONFIG_MODE_Pos)   |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_RESN_Bypass     << SAADC_CH_CONFIG_RESN_Pos)   |
      (SAADC_CH_CONFIG_RESP_Bypass     << SAADC_CH_CONFIG_RESP_Pos)   |
      (SAADC_CH_CONFIG_TACQ_40us       << SAADC_CH_CONFIG_TACQ_Pos);
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_VDD << SAADC_CH_PSELP_PSELP_Pos;
  NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC  << SAADC_CH_PSELN_PSELN_Pos;

  volatile int16_t result = 0;
  NRF_SAADC->RESULT.PTR    = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;

  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED) {}
  NRF_SAADC->EVENTS_STARTED = 0;

  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END) {}
  NRF_SAADC->EVENTS_END = 0;

  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED) {}
  NRF_SAADC->EVENTS_STOPPED = 0;

  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;

  if (result < 0) result = 0;
  return (float)result * 3.6f / 4096.0f;
}

// Estimate battery % for 2×AA alkaline (direct / unregulated).
// Returns 100 when on USB (VDD ≈ 3.3V regulated).
static uint8_t voltToPercent(float vdd) {
  const float V_FULL = 3.2f;   // 2 × 1.60V — fresh alkaline
  const float V_EMPTY = 2.0f;  // 2 × 1.00V — effectively dead
  if (vdd >= V_FULL)  return 100;
  if (vdd <= V_EMPTY) return 0;
  return (uint8_t)(100.0f * (vdd - V_EMPTY) / (V_FULL - V_EMPTY));
}

// =============================================================
// Bit-bang I2C
// =============================================================

static void sdaH() { pinMode(SDA_PIN, INPUT_PULLUP); }
static void sdaL() { pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, LOW); }
static void sclH() { pinMode(SCL_PIN, INPUT_PULLUP); }
static void sclL() { pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, LOW); }
static bool sdaR() { pinMode(SDA_PIN, INPUT_PULLUP); return digitalRead(SDA_PIN); }
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

// Returns true and fills tempC/rh on success
static bool readSHT40(float &tempC, float &rh) {
  bbStart();
  if (!bbWrite(SHT40_ADDR << 1)) { bbStop(); return false; }
  if (!bbWrite(0xFD))            { bbStop(); return false; }  // high-precision measure
  bbStop();

  delay(10);  // ~8.3 ms conversion time

  bbStart();
  if (!bbWrite((SHT40_ADDR << 1) | 1)) { bbStop(); return false; }
  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = bbRead(i < 5);
  bbStop();

  if (crc8(buf, 2)   != buf[2]) return false;
  if (crc8(buf+3, 2) != buf[5]) return false;

  uint16_t t_raw  = ((uint16_t)buf[0] << 8) | buf[1];
  uint16_t rh_raw = ((uint16_t)buf[3] << 8) | buf[4];
  tempC = -45.0f + 175.0f * (float)t_raw  / 65535.0f;
  rh    =  -6.0f + 125.0f * (float)rh_raw / 65535.0f;
  rh    = constrain(rh, 0.0f, 100.0f);
  return true;
}

// =============================================================
// BTHome v2 advertisement
//
// Service UUID:   0xFCD2
// Device info:    0x40  (version=2, no encryption, not trigger-based)
// Object 0x02:    temperature, sint16, factor 0.01 °C
// Object 0x03:    humidity,   uint16, factor 0.01 %
// =============================================================

static void doAdvertise(float tempC, float rh, float vdd, uint8_t batPct) {
  int16_t  t_enc = (int16_t)(tempC * 100.0f);
  uint16_t h_enc = (uint16_t)(rh   * 100.0f);
  uint16_t v_enc = (uint16_t)(vdd  * 1000.0f);  // millivolts (factor 0.001 V)

  // AD type 0x16 = Service Data – 16-bit UUID
  // BTHome v2 objects (must be sorted by object ID):
  //   0x01  battery %    uint8
  //   0x02  temperature  sint16  × 0.01 °C
  //   0x03  humidity     uint16  × 0.01 %
  //   0x0C  voltage      uint16  × 0.001 V
  uint8_t svc[] = {
    0xD2, 0xFC,                              // UUID 0xFCD2 little-endian
    0x40,                                    // BTHome v2, no encryption
    0x01,                                    // battery %
    batPct,
    0x02,                                    // temperature
    (uint8_t)( t_enc        & 0xFF),
    (uint8_t)((t_enc >> 8)  & 0xFF),
    0x03,                                    // humidity
    (uint8_t)( h_enc        & 0xFF),
    (uint8_t)((h_enc >> 8)  & 0xFF),
    0x0C,                                    // voltage
    (uint8_t)( v_enc        & 0xFF),
    (uint8_t)((v_enc >> 8)  & 0xFF)
  };

  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(0x16, svc, sizeof(svc));
  Bluefruit.ScanResponse.addName();

  // 100 ms interval → reduces power during advertising, still plenty for HA
  Bluefruit.Advertising.setInterval(160, 160);  // 160 × 0.625 ms = 100 ms
  Bluefruit.Advertising.setFastTimeout(0);      // stay in fast mode until stopped

  Bluefruit.Advertising.start(0);               // start (0 = no auto-stop)
  delay(ADV_MS);
  Bluefruit.Advertising.stop();
}

// =============================================================
// Power Management Functions
// =============================================================

static void disableUART() {
  // Disable UART to save ~1-2 mA during sleep
  // Note: We need to properly stop UART before disabling
  Serial.end();
  NRF_UARTE0->ENABLE = 0;
  // Set UART pins to low power state (input with pullup to prevent floating)
  nrf_gpio_cfg_input(25, NRF_GPIO_PIN_PULLUP);  // TX pin on Nice!Nano
  // Pin 24 is VCC_PIN, don't reconfigure it!
}

static void enableUART() {
  // Re-enable UART for debugging
  // Serial.begin() will handle the full re-initialization
}

static void disableUnusedPeripherals() {
  // Disable unused peripherals to minimize quiescent current
  NRF_TWIM0->ENABLE = 0;
  NRF_TWIM1->ENABLE = 0;
  NRF_SPIM0->ENABLE = 0;
  NRF_SPIM1->ENABLE = 0;
  NRF_SPIM2->ENABLE = 0;
  NRF_PWM0->ENABLE = 0;
  NRF_PWM1->ENABLE = 0;
  NRF_PWM2->ENABLE = 0;
}

static void configureLowPowerPins() {
  // Configure all unused pins as input with pullup to prevent floating
  // This reduces leakage current significantly
  
  // List of pins to configure (adjust based on your Nice!Nano pinout)
  // Exclude: VCC_PIN(24), GND_PIN(22), SCL_PIN(20), SDA_PIN(17), LED_BUILTIN
  const uint8_t unused_pins[] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 21, 23,
    26, 27, 28, 29, 30, 31
  };
  
  for (uint8_t i = 0; i < sizeof(unused_pins); i++) {
    nrf_gpio_cfg_input(unused_pins[i], NRF_GPIO_PIN_PULLUP);
  }
}

// Removed enterSystemOffWithRTC - using delay-based sleep instead

// =============================================================

void setup() {
  // CRITICAL: Wait 3 seconds at startup to allow bootloader entry
  // This prevents getting locked out if the code crashes or loops
  // During this time, you can double-tap reset to enter bootloader
  delay(3000);
  
  // Configure low power pins first to minimize startup current
  configureLowPowerPins();
  
  // Disable unused peripherals
  disableUnusedPeripherals();
  
  Serial.begin(115200);

  // Sensor power - keep OFF initially
  pinMode(GND_PIN, OUTPUT); digitalWrite(GND_PIN, LOW);
  pinMode(VCC_PIN, OUTPUT); digitalWrite(VCC_PIN, LOW);  // Start with sensor OFF

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  // BLE init (done once; Advertising is started/stopped each cycle)
  Bluefruit.autoConnLed(false);     // must be before begin() on some BSP versions
  Bluefruit.begin();
  Bluefruit.setTxPower(4);          // +4 dBm — reduced from +8 for power savings
  Bluefruit.setName("NanoTemp");    // shows in HA Bluetooth integration

  // Hard-take the LED pin so the BSP LED task can't blink it
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.println("\n=========================================");
  Serial.println("  BTHome Temp/Humidity Sensor  v2.1");
  Serial.println("  POWER OPTIMIZED - STABLE VERSION");
  Serial.print(  "  Interval : "); Serial.print(SLEEP_MS / 1000); Serial.println(" s");
  Serial.print(  "  Advertise: "); Serial.print(ADV_MS   / 1000); Serial.println(" s/cycle");
  Serial.println("  BLE name  : NanoTemp");
  Serial.println("  Sleep mode: WFE + UART off (~0.5-1 mA)");
  Serial.println("  In HA: Settings → Devices → Bluetooth");
  Serial.println("=========================================\n");
  Serial.flush();
}

void loop() {
  // ---- Power on sensor --------------------------------------
  digitalWrite(VCC_PIN, HIGH);
  delay(10);  // SHT40 power-up time

  // ---- Read sensor + supply voltage -------------------------
  float tempC, rh;
  bool ok = readSHT40(tempC, rh);
  float vdd    = readVDD();
  uint8_t bat  = voltToPercent(vdd);

  if (ok) {
    // Append to diagnostic log
    log_temp[log_idx] = tempC;
    log_rh[log_idx]   = rh;
    log_idx = (log_idx + 1) % LOG_SIZE;
    log_count++;

    float tempF = tempC * 9.0f / 5.0f + 32.0f;
    Serial.print("["); Serial.print(log_count); Serial.print("]  ");
    Serial.print(tempC, 2); Serial.print(" C / ");
    Serial.print(tempF, 2); Serial.print(" F   RH:");
    Serial.print(rh, 1);   Serial.print(" %   VDD:");
    Serial.print(vdd, 3);  Serial.print(" V   BAT:");
    Serial.print(bat);     Serial.println(" %");
    Serial.flush();

    // ---- Advertise -----------------------------------------
    doAdvertise(tempC, rh, vdd, bat);

  } else {
    Serial.println("ERROR: SHT40 read failed — skipping advertisement");
    Serial.flush();
  }

  // ---- Power off sensor for sleep ---------------------------
  digitalWrite(VCC_PIN, LOW);

  // ---- Sleep ------------------------------------------------
  Serial.print("Sleeping for ");
  Serial.print(SLEEP_MS / 1000);
  Serial.println(" s (UART off)...");
  Serial.flush();
  delay(100);  // Allow serial to finish
  
  // Disable UART to save ~1-2 mA during sleep
  disableUART();
  
  // Use delay() which calls sd_app_evt_wait() for low power
  // Achieves ~0.5-1 mA with UART disabled
  delay(SLEEP_MS);
  
  // Re-enable UART for next cycle
  Serial.begin(115200);
  delay(10);  // Allow UART to stabilize
}
