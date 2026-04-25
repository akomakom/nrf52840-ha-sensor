/*
 * Configuration file for nice!nano Zigbee Sensor
 * Edit these values to customize sensor behavior
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ── Device Identity ───────────────────────────────────────────────── */

/* Manufacturer and model name shown in Home Assistant
 * These appear in ZHA during pairing and in device info
 * Max length: 32 characters each */
#define DEVICE_MANUFACTURER     "DIY"
#define DEVICE_MODEL            "TempHumSensor"

/* ── Measurement & Poll Intervals ──────────────────────────────────── */

/* How often to poll the coordinator for messages (milliseconds)
 * Shorter = more responsive, higher power consumption
 * Longer = better battery life, slower response to commands
 * Recommended: 15000-60000 (15-60 seconds) */
#define POLL_INTERVAL_MS        60000

/* How often to read sensor and report values (milliseconds)
 * Must be >= POLL_INTERVAL_MS
 * Recommended: 60000-300000 (1-5 minutes) */
#define MEASUREMENT_INTERVAL_MS 60000

/* ── Battery Voltage Configuration ─────────────────────────────────── */

/* Battery voltage range for percentage calculation (millivolts)
 * MIN: voltage at 0% battery (device should sleep/shutdown)
 * MAX: voltage at 100% battery (fully charged)
 * 
 * IMPORTANT: nice!nano has onboard 3.3V regulator that requires ~3.5V minimum input.
 * The nRF52840 chip itself can run down to 1.7V, but the board's regulator cannot.
 * 
 * Recommended values for different battery types:
 * - Single LiPo (3.7V nominal):  MIN=3200, MAX=4200 (regulator dropout ~0.3V)
 * - 2xAAA alkaline (3.0V nominal): MIN=2400, MAX=3200 (marginal, may brownout)
 * - USB power (5V): Always reads ~3300mV (internal VDD measurement)
 * 
 * For LiPo batteries, set MIN=3200 to avoid brownout when battery is "empty" */
#define BATTERY_VOLTAGE_MIN_MV  2400
#define BATTERY_VOLTAGE_MAX_MV  3300

/* Battery voltage alarm threshold (millivolts)
 * When voltage drops below this, set alarm flag in Power Config cluster
 * ZHA will show low battery warning */
#define BATTERY_ALARM_MV        2200

/* ── Sensor Power Timing ───────────────────────────────────────────── */

/* Delay after powering on sensor before I2C communication (milliseconds)
 * SHT40 datasheet specifies 1ms typical, 20ms max
 * Increase if sensor readings are unreliable */
#define SENSOR_POWERUP_DELAY_MS 20

/* ── Watchdog Configuration ────────────────────────────────────────── */

/* Watchdog feed interval (milliseconds)
 * Adafruit bootloader starts 1s WDT that cannot be stopped
 * Must feed more frequently than 1000ms to prevent reset
 * Recommended: 800ms (safe margin, reduces wake-ups vs 500ms)
 * Lower values = more wake-ups = higher power consumption */
#define WATCHDOG_FEED_INTERVAL_MS 800

/* ── Zigbee Network Configuration ──────────────────────────────────── */

/* Erase Zigbee network credentials on boot
 * 1 = Erase NVRAM (fresh join every boot, for testing/pairing)
 * 0 = Preserve NVRAM (normal operation, rejoin existing network)
 * 
 * IMPORTANT: Set to 1 to reset any existing Zigbee pairing data, then change to 0 and rebuild
 * to preserve network credentials between reboots */
#define ERASE_PERSISTENT_STORAGE 0

/* ── Zigbee Network Configuration ──────────────────────────────────── */

/* Extended PAN ID (0 = join any network)
 * Set to specific value to restrict which coordinator to join
 * Format: 8-byte array, e.g. {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08} */
#define ZIGBEE_EXTENDED_PAN_ID  {0, 0, 0, 0, 0, 0, 0, 0}

/* ── Debug Options ─────────────────────────────────────────────────── */

/* Enable LED blink on measurement (debug mode only)
 * 0 = LED off, 1 = LED blinks briefly during sensor read */
#define DEBUG_LED_ON_MEASUREMENT 0

/* Enable verbose sensor logging (debug mode only)
 * 0 = minimal logs, 1 = detailed sensor readings */
#define DEBUG_VERBOSE_SENSOR     1

#endif /* CONFIG_H */
