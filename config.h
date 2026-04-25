/*
 * Configuration file for nice!nano Zigbee Sensor
 * Edit these values to customize sensor behavior
 */

#ifndef CONFIG_H
#define CONFIG_H

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
 * Common values:
 * - CR2032 coin cell: MIN=2000, MAX=3000
 * - LiPo battery:     MIN=3000, MAX=4200
 * - 2xAAA alkaline:   MIN=2000, MAX=3300 */
#define BATTERY_VOLTAGE_MIN_MV  2000
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
