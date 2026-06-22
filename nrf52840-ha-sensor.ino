// =============================================================
//  BTHome v2 BLE Temperature / Humidity Sensor
//  nRF52840 (Nice!Nano clone)
//
//  SHT40 wiring:  VCC=pin24  GND=pin22  SCL=pin20  SDA=pin17
//  Protocol:      BTHome v2 — Home Assistant auto-discovery
//
//  Sleep: delay() → sd_app_evt_wait() halts the CPU with the
//  SoftDevice idling at ~2-10 µA. True System-OFF (0.4 µA)
//  is not achievable with a software timer on nRF52840: RTC is
//  not a wakeup source from System-OFF; only GPIO/USB/NFC are.
//
//  Advertising sequence per cycle:
//    1. Legacy 1M PHY  (ADV_1M_MS)     — all BLE 4+ receivers,
//                                         HA BLE direct, ESP32-C6
//    2. Coded PHY S=8  (ADV_CODED_MS)  — extended range; needs
//                                         BLE 5 receiver (ESP32-C6)
//
//  S140 v6.1.1 supports only one advertising set at a time
//  (BLE_GAP_ADV_SET_COUNT_MAX=1). Both phases reuse handle 0:
//  Bluefruit owns it for phase 1; we reconfigure it via
//  SoftDevice directly for phase 2; Bluefruit reclaims it
//  next cycle by calling sd_ble_gap_adv_set_configure again.
//
//  Telemetry objects (BTHome v2, sorted by object ID):
//    0x01  battery %    uint8
//    0x02  temperature  sint16  × 0.01 °C
//    0x03  humidity     uint16  × 0.01 %
//    0x0C  voltage      uint16  × 0.001 V
// =============================================================

#include <bluefruit.h>
#include <string.h>

// ---- Pin assignments ----------------------------------------
#define VCC_PIN    24
#define GND_PIN    22
#define SCL_PIN    20
#define SDA_PIN    17
#define SHT40_ADDR 0x44

// ---- Timing -------------------------------------------------
#define SLEEP_MS       60000UL  // sleep between cycles (ms)
#define ADV_1M_MS       2000    // legacy 1M PHY window (ms)
#define ADV_CODED_MS    2000    // Coded PHY window (ms)

// ---- Diagnostic log (RAM retained across delay() sleep) -----
#define LOG_SIZE 8
static float    log_temp [LOG_SIZE];
static float    log_rh   [LOG_SIZE];
static uint8_t  log_idx   = 0;
static uint32_t log_count = 0;

// ---- Static buffer for Coded PHY adv data -------------------
// SoftDevice keeps a reference (not a copy); must remain valid
// while the advertising set is configured.
static uint8_t s_coded_ad[31];
static uint8_t s_coded_ad_len = 0;

// =============================================================
// Supply voltage via SAADC VDD channel
// GAIN=1/6, REFSEL=Internal(0.6V), 12-bit → full scale 3.6V
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

// 2×AA alkaline linear estimate; reads ~100% on USB (VDD=3.3V).
static uint8_t voltToPercent(float vdd) {
  const float V_FULL  = 3.2f;
  const float V_EMPTY = 2.0f;
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

static bool readSHT40(float &tempC, float &rh) {
  bbStart();
  if (!bbWrite(SHT40_ADDR << 1)) { bbStop(); return false; }
  if (!bbWrite(0xFD))            { bbStop(); return false; }
  bbStop();
  delay(10);
  bbStart();
  if (!bbWrite((SHT40_ADDR << 1) | 1)) { bbStop(); return false; }
  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = bbRead(i < 5);
  bbStop();
  if (crc8(buf, 2) != buf[2] || crc8(buf+3, 2) != buf[5]) return false;
  uint16_t t_raw  = ((uint16_t)buf[0] << 8) | buf[1];
  uint16_t rh_raw = ((uint16_t)buf[3] << 8) | buf[4];
  tempC = -45.0f + 175.0f * (float)t_raw  / 65535.0f;
  rh    =  -6.0f + 125.0f * (float)rh_raw / 65535.0f;
  rh    = constrain(rh, 0.0f, 100.0f);
  return true;
}

// =============================================================
// BTHome v2 service data builder (14 bytes into svc[])
// =============================================================
static void buildServiceData(uint8_t *svc, float tempC, float rh,
                              float vdd, uint8_t batPct) {
  int16_t  t_enc = (int16_t)(tempC * 100.0f);
  uint16_t h_enc = (uint16_t)(rh   * 100.0f);
  uint16_t v_enc = (uint16_t)(vdd  * 1000.0f);
  uint8_t p = 0;
  svc[p++] = 0xD2; svc[p++] = 0xFC;          // UUID 0xFCD2, LE
  svc[p++] = 0x40;                             // BTHome v2, no encryption
  svc[p++] = 0x01; svc[p++] = batPct;
  svc[p++] = 0x02;
  svc[p++] = (uint8_t)( t_enc       & 0xFF);
  svc[p++] = (uint8_t)((t_enc >> 8) & 0xFF);
  svc[p++] = 0x03;
  svc[p++] = (uint8_t)( h_enc       & 0xFF);
  svc[p++] = (uint8_t)((h_enc >> 8) & 0xFF);
  svc[p++] = 0x0C;
  svc[p++] = (uint8_t)( v_enc       & 0xFF);
  svc[p++] = (uint8_t)((v_enc >> 8) & 0xFF);  // p = 14
}

// =============================================================
// Phase 1: Legacy 1M PHY — scannable, name in scan response.
// Compatible with all BLE 4+ receivers and HA BLE direct.
// =============================================================
static void doLegacyAdvertise(float tempC, float rh, float vdd, uint8_t batPct) {
  uint8_t svc[14];
  buildServiceData(svc, tempC, rh, vdd, batPct);

  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(0x16, svc, sizeof(svc));
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.setInterval(160, 160);  // 160 × 0.625 ms = 100 ms
  Bluefruit.Advertising.setFastTimeout(0);
  Bluefruit.Advertising.start(0);
  delay(ADV_1M_MS);
  Bluefruit.Advertising.stop();
}

// =============================================================
// Phase 2: Coded PHY S=8 extended advertising.
// Extended range (≈4× vs 1M PHY); requires BLE 5 receiver.
// Name is embedded in the adv packet (no scan response for
// extended non-scannable). Uses static buffer s_coded_ad[].
//
// S140 v6.1.1 has BLE_GAP_ADV_SET_COUNT_MAX=1: only handle 0
// exists. We reconfigure it here after Bluefruit has stopped it;
// the next call to Bluefruit.Advertising.start() reconfigures
// it back to 1M PHY automatically.
// =============================================================
static void doCodedPHYAdvertise(float tempC, float rh, float vdd, uint8_t batPct) {
  uint8_t svc[14];
  buildServiceData(svc, tempC, rh, vdd, batPct);

  uint8_t p = 0;
  s_coded_ad[p++] = 1 + sizeof(svc);   // AD len = type(1) + svc(14) = 15
  s_coded_ad[p++] = 0x16;               // Service Data - 16-bit UUID
  memcpy(s_coded_ad + p, svc, sizeof(svc));
  p += sizeof(svc);                      // p = 16
  const char *name = "NanoTemp";
  uint8_t     nlen = strlen(name);
  s_coded_ad[p++] = 1 + nlen;           // 9
  s_coded_ad[p++] = 0x09;               // Complete Local Name
  memcpy(s_coded_ad + p, name, nlen);
  p += nlen;                             // p = 25
  s_coded_ad_len = p;

  ble_gap_adv_data_t adv_data = {};
  adv_data.adv_data.p_data = s_coded_ad;
  adv_data.adv_data.len    = s_coded_ad_len;

  ble_gap_adv_params_t adv_params = {};
  adv_params.properties.type = BLE_GAP_ADV_TYPE_EXTENDED_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  adv_params.primary_phy     = BLE_GAP_PHY_CODED;
  adv_params.secondary_phy   = BLE_GAP_PHY_CODED;
  adv_params.interval        = 160;  // 160 × 0.625 ms = 100 ms
  adv_params.duration        = 0;
  adv_params.max_adv_evts    = 0;

  uint8_t  handle = 0;
  uint32_t err = sd_ble_gap_adv_set_configure(&handle, &adv_data, &adv_params);
  if (err != NRF_SUCCESS) {
    Serial.print("Coded PHY configure err: 0x"); Serial.println(err, HEX);
    return;
  }
  err = sd_ble_gap_adv_start(handle, BLE_CONN_CFG_TAG_DEFAULT);
  if (err != NRF_SUCCESS) {
    Serial.print("Coded PHY start err: 0x"); Serial.println(err, HEX);
    return;
  }
  delay(ADV_CODED_MS);
  sd_ble_gap_adv_stop(handle);
}

// =============================================================

static void disableUnusedPeripherals() {
  NRF_TWIM0->ENABLE = 0;
  NRF_TWIM1->ENABLE = 0;
  NRF_SPIM0->ENABLE = 0;
  NRF_SPIM1->ENABLE = 0;
  NRF_SPIM2->ENABLE = 0;
  NRF_PWM0->ENABLE  = 0;
  NRF_PWM1->ENABLE  = 0;
  NRF_PWM2->ENABLE  = 0;
}

void setup() {
  // Safety window: double-tap reset enters bootloader while we wait.
  delay(3000);

  disableUnusedPeripherals();

  Serial.begin(115200);

  pinMode(GND_PIN, OUTPUT); digitalWrite(GND_PIN, LOW);
  pinMode(VCC_PIN, OUTPUT); digitalWrite(VCC_PIN, LOW);  // powered in loop()

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Bluefruit.autoConnLed(false);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);         // +4 dBm (reduced from +8 for power)
  Bluefruit.setName("NanoTemp");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.println("\n=========================================");
  Serial.println("  BTHome Temp/Humidity Sensor  v2.0");
  Serial.print(  "  Sleep     : "); Serial.print(SLEEP_MS    / 1000); Serial.println(" s");
  Serial.print(  "  Adv 1M    : "); Serial.print(ADV_1M_MS   / 1000); Serial.println(" s/cycle");
  Serial.print(  "  Adv Coded : "); Serial.print(ADV_CODED_MS / 1000); Serial.println(" s/cycle");
  Serial.println("  Sleep mode: sd_app_evt_wait (CPU halt, ~2-10 uA)");
  Serial.println("  In HA: Settings -> Devices -> Bluetooth");
  Serial.println("=========================================\n");
  Serial.flush();
}

void loop() {
  // ---- Power on sensor --------------------------------------
  digitalWrite(VCC_PIN, HIGH);
  delay(10);

  // ---- Read sensor + supply voltage -------------------------
  float tempC, rh;
  bool ok = readSHT40(tempC, rh);
  float   vdd = readVDD();
  uint8_t bat = voltToPercent(vdd);

  // ---- Power off sensor before radio activity ---------------
  // Radio heat during advertising can skew sensor readings if
  // the sensor stays powered next to the chip.
  digitalWrite(VCC_PIN, LOW);

  if (ok) {
    log_temp[log_idx] = tempC;
    log_rh  [log_idx] = rh;
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

    // ---- Advertise: 1M PHY then Coded PHY ------------------
    doLegacyAdvertise(tempC, rh, vdd, bat);
    doCodedPHYAdvertise(tempC, rh, vdd, bat);

  } else {
    Serial.println("ERROR: SHT40 read failed — skipping advertisement");
    Serial.flush();
  }

  // ---- Sleep ------------------------------------------------
  Serial.print("Sleeping "); Serial.print(SLEEP_MS / 1000); Serial.println(" s...");
  Serial.flush();
  delay(50);       // let UART TX drain

  Serial.end();    // disable UART (~1-2 mA saved during sleep)
  delay(SLEEP_MS);
  Serial.begin(115200);
  delay(10);
}
