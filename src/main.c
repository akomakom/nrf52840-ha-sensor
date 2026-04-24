/*
 * nice!nano — Zigbee Temperature / Humidity / Battery Sensor
 * nRF Connect SDK 2.7.0 / ZBOSS
 *
 * Hardware
 *   SHT40  I2C  (SDA=P0.17, SCL=P0.20)
 *          Power gate: VCC=P0.24 (OUTPUT), GND=P0.22 (OUTPUT)
 *   Battery voltage measured via SAADC internal VDD channel
 *
 * Zigbee
 *   ZHA profile (0x0104), Temperature Sensor device ID (0x0302)
 *   Server clusters: Basic | Power Config | Temp Measurement | Rel Humidity
 *   Sleepy End Device, 15-second poll / measurement interval
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_wdt.h>
#include <ram_pwrdn.h>

/* Feed all active watchdog channels.
 * The Adafruit UF2 bootloader starts a 1-second hardware WDT before
 * jumping to the application.  nRF WDT cannot be stopped once started,
 * so the app must keep feeding it.  Called every 500 ms from a Zephyr
 * timer that continues firing through System-ON deep sleep (RTC-backed). */
static void wdt_feed_all(void)
{
	if (nrf_wdt_started_check(NRF_WDT)) {
		for (int ch = 0; ch < 8; ch++) {
			if (nrf_wdt_reload_request_enable_check(NRF_WDT,
					(nrf_wdt_rr_register_t)ch)) {
				nrf_wdt_reload_request_set(NRF_WDT,
					(nrf_wdt_rr_register_t)ch);
			}
		}
	}
}

/* ── Sensor power pins ───────────────────────────────────────────── */

#define SHT40_VCC_PIN  24   /* P0.24 → sensor VCC */
#define SHT40_GND_PIN  22   /* P0.22 → sensor GND */
#define LED_PIN        15   /* P0.15 → blue LED (active HIGH) */

/* Power up the SHT40 before sht4x_init (POST_KERNEL 90) runs.
 * The driver sends a soft-reset over I2C during init; if VCC is low the
 * sensor has no power and the I2C transaction fails, leaving the device
 * not-ready.  Raw nrf_gpio HAL so this works before the GPIO driver is
 * fully configured by Zephyr. */
static int sht40_power_on(void)
{
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, SHT40_VCC_PIN));
	nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, SHT40_VCC_PIN));    /* VCC HIGH */
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, SHT40_GND_PIN));
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, SHT40_GND_PIN));  /* GND LOW */
	k_busy_wait(20000);   /* 20 ms — SHT40 power-on time before first I2C */
	return 0;
}
SYS_INIT(sht40_power_on, POST_KERNEL, 85);

/* ── ZBOSS / Zigbee ──────────────────────────────────────────────── */

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zboss_api_zdo.h>
#include <zb_mem_config_med.h>
#include <zigbee/zigbee_app_utils.h>
#include <zigbee/zigbee_error_handler.h>
#include <zb_nrf_platform.h>        /* zigbee_enable() */

/* zb_bdb_reset_via_local_action is exported from the prebuilt libzboss.a
 * but not declared in any public header in NCS 2.7.  It is used the same
 * way inside zigbee_app_utils.c (the NCS factory-reset implementation). */
extern void zb_bdb_reset_via_local_action(zb_uint8_t param);

/* ZCL cluster headers */
#include <zcl/zb_zcl_basic.h>
#include <zcl/zb_zcl_power_config.h>
#include <zcl/zb_zcl_temp_measurement.h>
#include <zcl/zb_zcl_rel_humidity_measurement.h>

LOG_MODULE_REGISTER(sensor_app, LOG_LEVEL_INF);

/* ── Device tree handles ─────────────────────────────────────────── */

#define SHT40_NODE DT_NODELABEL(sht40)
#define GPIO0_NODE DT_NODELABEL(gpio0)

static const struct device *sht40_dev = DEVICE_DT_GET(SHT40_NODE);
static const struct device *gpio0_dev = DEVICE_DT_GET(GPIO0_NODE);

static const struct adc_dt_spec adc_vdd =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/* ── Zigbee endpoint config ──────────────────────────────────────── */

#define SENSOR_ENDPOINT          1
#define SENSOR_IN_CLUSTER_COUNT  4
#define SENSOR_OUT_CLUSTER_COUNT 0
#define SENSOR_REPORT_COUNT      4   /* temp + humidity + batt % + batt voltage */

/* ZHA Temperature Sensor device ID */
#define SENSOR_DEVICE_ID  0x0302u

/* Measurement interval.
 * 60 s is a reasonable production value; shorten for bench testing only.
 * Note: shortening this does NOT lower idle current — the radio's sleep
 * behaviour is governed by ZBOSS, not by how often we schedule a reading. */
#define MEAS_INTERVAL_MS         60000
#define MEAS_INTERVAL_TICKS      ZB_MILLISECONDS_TO_BEACON_INTERVAL(MEAS_INTERVAL_MS)

/* ── ZCL attribute storage ───────────────────────────────────────── */

/* Basic cluster */
static zb_uint8_t attr_zcl_version    = ZB_ZCL_VERSION;
static zb_uint8_t attr_power_source   = ZB_ZCL_BASIC_POWER_SOURCE_BATTERY;
static char       attr_manufacturer[] = "DIY";
static char       attr_model[]        = "TempHumSensor";

/* Power Config cluster
 *   voltage    : 100 mV units  (30 → 3.0 V)
 *   percentage : 0.5 % units   (200 → 100 %)
 */
static zb_uint8_t attr_batt_voltage    = 30;
static zb_uint8_t attr_batt_percentage = 200;

/* Temperature Measurement cluster  (0.01 °C units, sint16) */
static zb_int16_t  attr_temp_value     = ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_UNKNOWN;
static zb_int16_t  attr_temp_min_value = -4000;
static zb_int16_t  attr_temp_max_value =  8500;
static zb_uint16_t attr_temp_tolerance =     0;

/* Relative Humidity Measurement cluster  (0.01 % units, uint16) */
static zb_uint16_t attr_hum_value     = 0;
static zb_uint16_t attr_hum_min_value = 0;
static zb_uint16_t attr_hum_max_value = 10000;

/* ── Attribute lists ─────────────────────────────────────────────── */

ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(basic_attr_list, ZB_ZCL_BASIC)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID,        &attr_zcl_version)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,   attr_manufacturer)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,    attr_model)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID,       &attr_power_source)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(power_config_attr_list,
						   ZB_ZCL_POWER_CONFIG)
ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
			&attr_batt_voltage,
			ZB_ZCL_ATTR_TYPE_U8,
			ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING)
ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
			&attr_batt_percentage,
			ZB_ZCL_ATTR_TYPE_U8,
			ZB_ZCL_ATTR_ACCESS_READ_ONLY | ZB_ZCL_ATTR_ACCESS_REPORTING)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

ZB_ZCL_DECLARE_TEMP_MEASUREMENT_ATTRIB_LIST(temp_attr_list,
	&attr_temp_value,
	&attr_temp_min_value,
	&attr_temp_max_value,
	&attr_temp_tolerance);

ZB_ZCL_DECLARE_REL_HUMIDITY_MEASUREMENT_ATTRIB_LIST(hum_attr_list,
	&attr_hum_value,
	&attr_hum_min_value,
	&attr_hum_max_value);

/* ── Cluster list ────────────────────────────────────────────────── */

/* ZB_DECLARE_SIMPLE_DESC uses ## so macros would NOT be expanded — use literals. */
ZB_DECLARE_SIMPLE_DESC(4, 0);

static zb_zcl_cluster_desc_t sensor_clusters[] = {
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_ARRAY_SIZE(basic_attr_list, zb_zcl_attr_t),
		(zb_zcl_attr_t *)basic_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_ARRAY_SIZE(power_config_attr_list, zb_zcl_attr_t),
		(zb_zcl_attr_t *)power_config_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_ARRAY_SIZE(temp_attr_list, zb_zcl_attr_t),
		(zb_zcl_attr_t *)temp_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
		ZB_ZCL_ARRAY_SIZE(hum_attr_list, zb_zcl_attr_t),
		(zb_zcl_attr_t *)hum_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID),
};

/* ── Simple descriptor ───────────────────────────────────────────── */

ZB_AF_SIMPLE_DESC_TYPE(SENSOR_IN_CLUSTER_COUNT, SENSOR_OUT_CLUSTER_COUNT)
	sensor_simple_desc = {
	SENSOR_ENDPOINT,
	ZB_AF_HA_PROFILE_ID,
	SENSOR_DEVICE_ID,
	0,   /* device version */
	0,   /* reserved */
	SENSOR_IN_CLUSTER_COUNT,
	SENSOR_OUT_CLUSTER_COUNT,
	{
		ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
		ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
	}
};

/* ── Reporting context + endpoint ────────────────────────────────── */

static zb_zcl_reporting_info_t sensor_report_ctx[SENSOR_REPORT_COUNT];

ZB_AF_DECLARE_ENDPOINT_DESC(sensor_ep,
	SENSOR_ENDPOINT,
	ZB_AF_HA_PROFILE_ID,
	0,
	NULL,
	ZB_ZCL_ARRAY_SIZE(sensor_clusters, zb_zcl_cluster_desc_t),
	sensor_clusters,
	(zb_af_simple_desc_1_1_t *)&sensor_simple_desc,
	SENSOR_REPORT_COUNT, sensor_report_ctx,
	0, NULL);

ZBOSS_DECLARE_DEVICE_CTX_1_EP(sensor_device_ctx, sensor_ep);

/* ── ADC state ───────────────────────────────────────────────────── */

static int16_t           adc_raw_val;
static struct adc_sequence adc_seq = {
	.buffer      = &adc_raw_val,
	.buffer_size = sizeof(adc_raw_val),
};

/* ── Forward declarations ────────────────────────────────────────── */
static void measure_and_report(zb_uint8_t param);
static void restore_long_poll(zb_uint8_t param);
static void on_network_joined(void);

/* ── Post-join interview window ──────────────────────────────────── */

/* After joining, poll every 2 s for 5 min so ZHA can complete its
 * attribute interview, then restore the normal sleep cycle.
 * 5 minutes is needed because ZHA sends configure-reporting frames for
 * every reportable attribute; at 60 s long poll each frame takes up to
 * 60 s to deliver, so 4 attributes × 60 s = 4 min minimum.  Using 300 s
 * gives margin for slow coordinators / congested networks. */
static void on_network_joined(void)
{
	zb_zdo_pim_set_long_poll_interval(2000);
	ZB_SCHEDULE_APP_ALARM_CANCEL(restore_long_poll, ZB_ALARM_ANY_PARAM);
	ZB_SCHEDULE_APP_ALARM(restore_long_poll, 0,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(300000));
	ZB_SCHEDULE_APP_ALARM_CANCEL(measure_and_report, ZB_ALARM_ANY_PARAM);
	ZB_SCHEDULE_APP_ALARM(measure_and_report, 0,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(500));
}

static void restore_long_poll(zb_uint8_t param)
{
	ARG_UNUSED(param);
	zb_zdo_pim_set_long_poll_interval(MEAS_INTERVAL_MS);
	LOG_INF("Interview window closed — poll interval %d s",
		MEAS_INTERVAL_MS / 1000);
}

/* ── Battery measurement ─────────────────────────────────────────── */

static void update_battery(void)
{
	int err = adc_read(adc_vdd.dev, &adc_seq);

	if (err) {
		LOG_WRN("ADC read error: %d", err);
		return;
	}

	int32_t mv = adc_raw_val;

	adc_raw_to_millivolts_dt(&adc_vdd, &mv);
	LOG_INF("VDD: %d mV", (int)mv);

	/* ZCL battery voltage: 100 mV units */
	zb_uint8_t zcl_voltage = (zb_uint8_t)(mv / 100);

	/* ZCL BatteryPercentageRemaining: 0.5 % units (0–200).
	 * Linear approximation: 2100 mV = 0 %, 3300 mV = 100 % */
	int32_t pct = ((mv - 2100) * 200) / (3300 - 2100);

	if (pct < 0) {
		pct = 0;
	} else if (pct > 200) {
		pct = 200;
	}

	zb_uint8_t zcl_percentage = (zb_uint8_t)pct;

	ZB_ZCL_SET_ATTRIBUTE(SENSOR_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
		&zcl_voltage, ZB_FALSE);

	ZB_ZCL_SET_ATTRIBUTE(SENSOR_ENDPOINT,
		ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
		&zcl_percentage, ZB_FALSE);
}

/* ── LED heartbeat ───────────────────────────────────────────────── */

/* Brief blink on the blue LED — used to show the device is alive while
 * running on battery with USB/logging disabled.  Two quick flashes per
 * measurement cycle = "woke up, read sensor, sent Zigbee report". */
static void led_blink(int count)
{
	for (int i = 0; i < count; i++) {
		gpio_pin_set(gpio0_dev, LED_PIN, 1);
		k_sleep(K_MSEC(30));
		gpio_pin_set(gpio0_dev, LED_PIN, 0);
		if (i < count - 1) {
			k_sleep(K_MSEC(120));
		}
	}
}

/* ── Measurement + report (ZBOSS scheduler callback) ─────────────── */

static void measure_and_report(zb_uint8_t param)
{
	ARG_UNUSED(param);

	/* Power up the SHT40 */
	gpio_pin_set(gpio0_dev, SHT40_VCC_PIN, 1);
	gpio_pin_set(gpio0_dev, SHT40_GND_PIN, 0);
	k_sleep(K_MSEC(10));   /* SHT40 power-on time */

	/* Read sensor */
	int err = sensor_sample_fetch(sht40_dev);

	if (err) {
		LOG_ERR("SHT40 fetch error: %d", err);
	} else {
		struct sensor_value temp_sv, hum_sv;

		sensor_channel_get(sht40_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_sv);
		sensor_channel_get(sht40_dev, SENSOR_CHAN_HUMIDITY,     &hum_sv);

		/* Convert to ZCL units (0.01 per unit) */
		zb_int16_t zcl_temp =
			(zb_int16_t)(temp_sv.val1 * 100 + temp_sv.val2 / 10000);
		zb_uint16_t zcl_hum =
			(zb_uint16_t)(hum_sv.val1 * 100 + hum_sv.val2 / 10000);

		LOG_INF("Temp: %d.%02d °C   Hum: %d.%02d %%",
			temp_sv.val1, abs(temp_sv.val2 / 10000),
			hum_sv.val1,  hum_sv.val2 / 10000);

		ZB_ZCL_SET_ATTRIBUTE(SENSOR_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
			(zb_uint8_t *)&zcl_temp, ZB_FALSE);

		ZB_ZCL_SET_ATTRIBUTE(SENSOR_ENDPOINT,
			ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
			ZB_ZCL_CLUSTER_SERVER_ROLE,
			ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
			(zb_uint8_t *)&zcl_hum, ZB_FALSE);

		/* Two quick blinks = alive + reported */
		led_blink(2);
	}

	/* Power down the SHT40 */
	gpio_pin_set(gpio0_dev, SHT40_VCC_PIN, 0);
	gpio_pin_set(gpio0_dev, SHT40_GND_PIN, 0);

	/* Battery telemetry */
	update_battery();

	/* Schedule next measurement */
	ZB_SCHEDULE_APP_ALARM(measure_and_report, 0, MEAS_INTERVAL_TICKS);
}

/* ── ZBOSS signal handler ────────────────────────────────────────── */

void zboss_signal_handler(zb_bufid_t bufid)
{
	zb_zdo_app_signal_hdr_t  *hdr    = NULL;
	zb_zdo_app_signal_type_t  sig    = zb_get_app_signal(bufid, &hdr);
	zb_ret_t                  status = ZB_GET_APP_SIGNAL_STATUS(bufid);

	switch (sig) {
	case ZB_BDB_SIGNAL_DEVICE_FIRST_START:
		/* ZBOSS initialized with empty NVRAM — default handler starts
		 * network steering automatically. */
		LOG_INF("First start — beginning network steering");
		break;

	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
		/* Reboot with existing network credentials in NVRAM. */
		if (status == RET_OK) {
			/* Open the same 60 s interview window used after a fresh
			 * join.  This is needed because the ZBOSS ZCL attribute-
			 * reporting configuration is stored separately from
			 * network credentials in NVRAM.  If that config is lost
			 * (e.g. after a partition-map change) ZHA must re-send
			 * configure-reporting frames for every reportable
			 * attribute.  At 60 s long poll each frame takes up to
			 * 60 s to reach the device; at 2 s poll all frames
			 * arrive within the 60 s window.
			 *
			 * Once the interview is complete and ZBOSS has the
			 * reporting config back in NVRAM, ZHA stays silent and
			 * the device can sleep properly between measurements. */
			LOG_INF("Rejoined network — opening 300 s interview window");
			on_network_joined();
		} else {
			/* Coordinator temporarily unreachable (HA offline, etc.).
			 * Do NOT wipe NVRAM here — that would cause an infinite
			 * wipe → FIRST_START → steer → fail → wipe loop that keeps
			 * the radio scanning channels and draws ~38 mA constantly.
			 *
			 * Instead let the default handler's built-in retry run:
			 * it retries for ~200 s then stops and enters a low-power
			 * wait state.  NVRAM credentials are preserved so the
			 * device can rejoin when the coordinator comes back online
			 * (next power cycle).  Use the serial menu to force re-pair
			 * if the network is permanently gone. */
			LOG_WRN("Rejoin failed — retrying for up to ~200 s, then sleeping");
		}
		break;

	case ZB_BDB_SIGNAL_STEERING:
		if (status == RET_OK) {
			LOG_INF("Network steering OK — starting interview window");
			on_network_joined();
		} else {
			/* Timed out — ZHA wasn't in permit-join mode.
			 * Default handler retries with back-off; just log here.
			 * Do NOT call user_input_indicate(): it has an internal
			 * ZB_ERROR_CHECK that can reset the device. */
			LOG_WRN("Steering attempt failed — will retry");
		}
		break;

	case ZB_COMMON_SIGNAL_CAN_SLEEP:
		/* ZBOSS is idle between polls — the default handler below calls
		 * zb_sleep_now() → zb_osif_sleep() → k_sleep(), letting Zephyr
		 * PM enter System-ON deep sleep on nRF52840.
		 *
		 * CRITICAL: do NOT log this signal.  It fires every poll cycle
		 * (every ~100 ms during short-poll mode after a data exchange),
		 * generating hundreds of messages.  Each message queues work on
		 * the USB CDC-ACM log backend's system workqueue; even on battery
		 * with no USB cable the workqueue handler runs, keeping the CPU
		 * active and preventing Zephyr PM from engaging.  Suppressing the
		 * log here is what allows the device to reach <1 mA sleep. */
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		LOG_INF("Left Zigbee network");
		ZB_SCHEDULE_APP_ALARM_CANCEL(measure_and_report,
					     ZB_ALARM_ANY_PARAM);
		break;

	default:
		LOG_INF("Zigbee signal 0x%x status %d (unhandled)",
			(unsigned)sig, (int)status);
		break;
	}

	/* No ZB_ERROR_CHECK wrapper — a transient non-RET_OK from the default
	 * handler would trigger a ZBOSS assert → reset loop with
	 * ZBOSS_RESET_ON_ASSERT=y. */
	zigbee_default_signal_handler(bufid);

	/* Return the buffer to the ZBOSS pool.  Without this every signal
	 * leaks one buffer; pool exhaustion silently prevents steering. */
	if (bufid) {
		zb_buf_free(bufid);
	}
}

/* ── WDT keepalive ───────────────────────────────────────────────── */

/* Feeds the bootloader WDT every 500 ms.  Zephyr kernel timers are
 * backed by the RTC and continue firing through System-ON deep sleep,
 * so this works correctly even when the CPU is sleeping between polls. */
static void wdt_keepalive_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	wdt_feed_all();
}
static K_TIMER_DEFINE(wdt_keepalive_timer, wdt_keepalive_cb, NULL);

/* ── main ────────────────────────────────────────────────────────── */

int main(void)
{
	int err;

	/* Start WDT keepalive immediately — must feed within 1 s of boot */
	k_timer_start(&wdt_keepalive_timer, K_MSEC(500), K_MSEC(500));

	/* Brief pause for USB CDC to enumerate so early logs are captured.
	 * With PM enabled, deep sleep only engages when USB is disconnected
	 * (no VBUS → USB oscillator stops → nRF52840 can enter System-ON
	 * sleep).  Measuring battery current: unplug USB after this log. */
	k_sleep(K_MSEC(500));
	LOG_INF("Sensor app starting (interval %d s)", MEAS_INTERVAL_MS / 1000);

	/* ── Verify devices are ready ── */
	if (!device_is_ready(sht40_dev)) {
		LOG_ERR("SHT40 not ready — check I2C wiring");
		return -ENODEV;
	}
	if (!device_is_ready(gpio0_dev)) {
		LOG_ERR("GPIO0 not ready");
		return -ENODEV;
	}
	if (!adc_is_ready_dt(&adc_vdd)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	/* ── GPIO: sensor power pins (both off initially) ── */
	err = gpio_pin_configure(gpio0_dev, SHT40_VCC_PIN,
				 GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("VCC pin configure: %d", err);
		return err;
	}
	err = gpio_pin_configure(gpio0_dev, SHT40_GND_PIN,
				 GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("GND pin configure: %d", err);
		return err;
	}
	err = gpio_pin_configure(gpio0_dev, LED_PIN, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("LED pin configure: %d", err);
		return err;
	}

	/* ── ADC: configure the VDD channel ── */
	err = adc_channel_setup_dt(&adc_vdd);
	if (err) {
		LOG_ERR("ADC channel setup: %d", err);
		return err;
	}
	adc_sequence_init_dt(&adc_vdd, &adc_seq);

	/* ── Zigbee: register endpoint and start stack ── */
	ZB_AF_REGISTER_DEVICE_CTX(&sensor_device_ctx);

	/* Power down idle SRAM banks before enabling the radio.
	 * Must be called explicitly — the library does not auto-call this
	 * unless CONFIG_RAM_POWER_ADJUST_ON_HEAP_RESIZE=y (requires newlib). */
	power_down_unused_ram();

	/* Enable sleepy end device behavior (radio off between polls).
	 * Must be called before zigbee_enable(). */
	zigbee_configure_sleepy_behavior(true);

	/* To re-pair: remove device in ZHA, then send a factory-reset
	 * command via the serial menu (to be added) or power-cycle with
	 * zigbee_erase_persistent_storage(ZB_TRUE) temporarily re-enabled. */
	zigbee_enable();

	return 0;
}
