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

/* ── Boot diagnostic via blue LED (P0.15) ────────────────────────────
 *
 * Step 1: Show reset cause (runs at PRE_KERNEL_1 priority 0, first thing):
 *   WDT reset  → 5 rapid flashes (50 ms each), then 1 s pause
 *   CPU lockup → 3 long flashes (400 ms each), then 1 s pause
 *   Other      → no preamble, proceed directly to stage blinks
 *
 * Step 2: Stage blink codes (LED always OFF between groups in PK1/PK2):
 *   1 flash  → PK1 priority 0 passed  (crash in PK1 prio 0–20)
 *   2 flashes → PK1 priority 20 passed (crash at MPSL init ~prio 40)
 *   3 flashes → PK1 priority 50 passed (crash prio 50–80)
 *   4 flashes → PK1 priority 80 passed (crash prio 80 – PK2)
 *   5 flashes → PK2 priority 0 passed
 *   6 flashes → PK2 priority 50 passed
 *   7 flashes → PK2 priority 90 passed (crash near end of PK2)
 *
 * Step 3: POST_KERNEL (LED stays ON solid after group):
 *   1+solid → PK prio 0   (crash prio 0–40)
 *   2+solid → PK prio 40  (crash prio 40–60)
 *   3+solid → PK prio 60  (crash prio 60–80)
 *   4+solid → PK prio 80  (crash prio 80–90)
 *   5+solid → PK prio 90  (crash prio 90–91, in usb_device_init / sht4x_init)
 *   6+solid → PK prio 91  (crash prio 91–95, nrf5_init at 95)
 *   7+solid → PK prio 95  (crash prio 95–99, nrf_802154_configure at 99)
 *   LED OFF in main() = main() reached, Zigbee starting
 *
 * WDT feeding: every checkpoint feeds active WDT channels so a 1-second
 * Adafruit bootloader WDT cannot fire before we can gather diagnostic info.
 */
#include <helpers/nrfx_reset_reason.h>
#include <hal/nrf_wdt.h>

#define DIAG_LED_PIN 15   /* P0.15 = blue LED on nice!nano */

/* Feed all active watchdog channels to prevent bootloader WDT timeout */
static void diag_pet_wdt(void)
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

/* N quick blinks, LED ends OFF (pre-PK stages) */
static void diag_flash_off(int n)
{
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
	diag_pet_wdt();
	for (int i = 0; i < n; i++) {
		nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(120000);  /* 120 ms on */
		nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(250000);  /* 250 ms off */
		diag_pet_wdt();
	}
	k_busy_wait(400000);  /* 400 ms between groups */
}

/* N blinks then LED stays ON (POST_KERNEL stages) */
static void diag_flash_on(int n)
{
	diag_pet_wdt();
	for (int i = 0; i < n; i++) {
		nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(120000);
		nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(250000);
		diag_pet_wdt();
	}
	nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));  /* stays ON */
}

/* Show reset cause first, so we know WHY we're in a reset loop.
 * Runs at PRE_KERNEL_1 priority 0 — very first thing the app does.
 *
 * FIRMWARE SIGNATURE: two slow 800 ms pulses before anything else.
 * If you see these two long flashes at power-on, new firmware is loaded. */
static int diag_reset_cause(void)
{
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
	diag_pet_wdt();

	/* Two long pulses = new firmware confirmation */
	for (int i = 0; i < 2; i++) {
		nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(800000);   /* 800 ms ON */
		nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
		k_busy_wait(400000);   /* 400 ms OFF */
		diag_pet_wdt();
	}
	k_busy_wait(500000);  /* 500 ms pause before reset-cause display */
	diag_pet_wdt();

	uint32_t reas = nrfx_reset_reason_get();
	nrfx_reset_reason_clear(0xFFFFFFFF);

	if (reas & NRFX_RESET_REASON_DOG_MASK) {
		/* WDT fired: 5 rapid blinks then pause — bootloader WDT! */
		for (int i = 0; i < 5; i++) {
			nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
			k_busy_wait(60000);
			nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
			k_busy_wait(60000);
		}
		k_busy_wait(800000);
	} else if (reas & NRFX_RESET_REASON_LOCKUP_MASK) {
		/* CPU lockup (hard fault): 3 slow blinks then pause */
		for (int i = 0; i < 3; i++) {
			nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
			k_busy_wait(400000);
			nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
			k_busy_wait(200000);
		}
		k_busy_wait(800000);
	}
	/* Any other cause (pin reset, software reset) → no preamble */
	return 0;
}
SYS_INIT(diag_reset_cause, PRE_KERNEL_1, 0);

/* ── PRE_KERNEL_1 checkpoints ── */
static int diag_pk1_1(void)  { diag_flash_off(1); return 0; }
static int diag_pk1_20(void) { diag_flash_off(2); return 0; }
/* MPSL init (mpsl_lib_init_sys) runs at PRE_KERNEL_1 ~priority 40 */
static int diag_pk1_50(void) { diag_flash_off(3); return 0; }
static int diag_pk1_80(void) { diag_flash_off(4); return 0; }

SYS_INIT(diag_pk1_1,  PRE_KERNEL_1,  1);
SYS_INIT(diag_pk1_20, PRE_KERNEL_1, 20);
SYS_INIT(diag_pk1_50, PRE_KERNEL_1, 50);
SYS_INIT(diag_pk1_80, PRE_KERNEL_1, 80);

/* ── PRE_KERNEL_2 checkpoints ── */
static int diag_pk2_0(void)  { diag_flash_off(5); return 0; }
static int diag_pk2_50(void) { diag_flash_off(6); return 0; }
static int diag_pk2_90(void) { diag_flash_off(7); return 0; }

SYS_INIT(diag_pk2_0,  PRE_KERNEL_2,  0);
SYS_INIT(diag_pk2_50, PRE_KERNEL_2, 50);
SYS_INIT(diag_pk2_90, PRE_KERNEL_2, 90);

/* ── POST_KERNEL checkpoints — LED stays ON solid after each ──
 *
 * Priority bracket → what runs between each pair:
 *   0   – logger init, usb_work_q, mpsl_low_prio
 *  40   – k_sys_work_q
 *  60   – saadc init, flash/nvs init, i2c init
 *  80   – usb_init (hardware)
 *  89   – nrf5_init (IEEE 802.15.4 driver)   ← CONFIG_IEEE802154_NRF5_INIT_PRIO=89
 *  90   – usb_device_init (CDC ACM), net_core_init, sensor inits
 *  99   – nrf_802154_configure             ← CONFIG_NRF_802154_RADIO_CONFIG_PRIO=99
 */
static int diag_post_0(void)  { diag_flash_on(1); return 0; }
static int diag_post_40(void) { diag_flash_on(2); return 0; }
static int diag_post_60(void) { diag_flash_on(3); return 0; }
static int diag_post_80(void) { diag_flash_on(4); return 0; }
/* Priority 88: runs between usb_init (80) and usb_device_init/sht4x_init (90).
 * Feeds WDT aggressively and shows a 2-second solid LED so it's unmistakable.
 * If you see "4+solid → 2s solid → ..." the crash is at priority 90+.
 * If you see "4+solid → reset" this function never ran → crash at 81-87. */
static int diag_post_88(void)
{
	/* Feed WDT — this is the critical gap where bootloader WDT was firing */
	diag_pet_wdt();
	k_busy_wait(50000);
	diag_pet_wdt();
	/* 2-second solid LED = unmistakable "reached priority 88" signal */
	nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
	k_busy_wait(500000); diag_pet_wdt();
	k_busy_wait(500000); diag_pet_wdt();
	k_busy_wait(500000); diag_pet_wdt();
	k_busy_wait(500000); diag_pet_wdt();
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));
	k_busy_wait(300000);
	return 0;
}

static int diag_post_90(void) { diag_flash_on(5); return 0; }

/* Priority 91: Runs after usb_device_init/sht4x_init/temp_nrf5_mpsl_init (all at 90).
 * 6 blinks = those inits survived.  No 6-blinks = crash is inside priority-90 inits. */
static int diag_post_91(void) { diag_flash_on(6); return 0; }

/* Priority 92: diagnostic complete — USB CDC is up, proceed to boot. */
static int diag_usb_wait(void) { diag_pet_wdt(); return 0; }

static int diag_post_95(void) { diag_flash_on(7); return 0; }

SYS_INIT(diag_post_0,  POST_KERNEL,  0);

SYS_INIT(diag_post_40, POST_KERNEL, 40);
SYS_INIT(diag_post_60, POST_KERNEL, 60);
SYS_INIT(diag_post_80, POST_KERNEL, 80);
SYS_INIT(diag_post_88, POST_KERNEL, 88);
SYS_INIT(diag_post_90, POST_KERNEL, 90);
SYS_INIT(diag_post_91, POST_KERNEL, 91);
SYS_INIT(diag_usb_wait, POST_KERNEL, 92);
SYS_INIT(diag_post_95, POST_KERNEL, 95);

/* ZBOSS / Zigbee */
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

/* ── Sensor power pins (defined early — used by sht40_power_on) ─── */
#define SHT40_VCC_PIN  24   /* P0.24 → sensor VCC */
#define SHT40_GND_PIN  22   /* P0.22 → sensor GND */

/* Power up the SHT40 before sht4x_init (POST_KERNEL 90) runs.
 * The driver sends a soft-reset over I2C during init; if VCC is low the
 * sensor has no power and the I2C transaction fails, leaving the device
 * not-ready.  Using raw nrf_gpio HAL so this works before the GPIO
 * driver is fully configured. */
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
#define SENSOR_REPORT_COUNT      4   /* temp + humidity + batt% + batt voltage */

/* ZHA Temperature Sensor device ID */
#define SENSOR_DEVICE_ID  0x0302u

/* Measurement interval: 15 seconds in ZBOSS beacon intervals */
#define MEAS_INTERVAL_TICKS  ZB_MILLISECONDS_TO_BEACON_INTERVAL(15000)

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

/* Declare only the attributes we have values for — ZHA reads what's
 * available and skips the rest.  Avoids NULL-pointer dereferences from
 * the _EXT macro's full list. */
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(basic_attr_list, ZB_ZCL_BASIC)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID,        &attr_zcl_version)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,   attr_manufacturer)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,    attr_model)
ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID,       &attr_power_source)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

/*
 * Power Config: build manually with ZB_ZCL_SET_ATTR_DESC_M to avoid
 * the two-argument battery-number macros that ZB_ZCL_SET_ATTR_DESC expands into.
 */
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

static int16_t          adc_raw_val;
static struct adc_sequence adc_seq = {
	.buffer      = &adc_raw_val,
	.buffer_size = sizeof(adc_raw_val),
};

/* ── Forward declarations ────────────────────────────────────────── */
static void measure_and_report(zb_uint8_t param);
static void restore_long_poll(zb_uint8_t param);
static void on_network_joined(void);

/* After joining, poll every 2 s for 60 s so ZHA can complete its
 * attribute interview, then restore the normal 15-second sleep cycle. */
static void on_network_joined(void)
{
	zb_zdo_pim_set_long_poll_interval(2000);
	ZB_SCHEDULE_APP_ALARM_CANCEL(restore_long_poll, ZB_ALARM_ANY_PARAM);
	ZB_SCHEDULE_APP_ALARM(restore_long_poll, 0,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(60000));
	ZB_SCHEDULE_APP_ALARM_CANCEL(measure_and_report, ZB_ALARM_ANY_PARAM);
	ZB_SCHEDULE_APP_ALARM(measure_and_report, 0,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(500));
}

static void restore_long_poll(zb_uint8_t param)
{
	ARG_UNUSED(param);
	zb_zdo_pim_set_long_poll_interval(15000);
	LOG_INF("Interview window closed — poll interval 15 s");
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

	/* ZCL BatteryPercentageRemaining: 0.5 % units (0-200).
	 * Linear: 2100 mV = 0 %, 3300 mV = 100 % */
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
		 * network steering.  Do NOT start measurements yet. */
		LOG_INF("First start — beginning network steering");
		break;

	case ZB_BDB_SIGNAL_DEVICE_REBOOT:
		/* Reboot with existing network credentials in NVRAM. */
		if (status == RET_OK) {
			/* Rejoined old network — go straight to normal sleep
			 * cycle; ZHA already interviewed us on first join. */
			LOG_INF("Rejoined network — starting measurements");
			zb_zdo_pim_set_long_poll_interval(15000);
			ZB_SCHEDULE_APP_ALARM_CANCEL(measure_and_report,
						     ZB_ALARM_ANY_PARAM);
			ZB_SCHEDULE_APP_ALARM(measure_and_report, 0,
				ZB_MILLISECONDS_TO_BEACON_INTERVAL(500));
		} else {
			/* The network we were on is gone (coordinator reset,
			 * ZHA removed us, etc.).  Wipe NVRAM so the next boot
			 * starts a clean steer; ZHA can then discover us fresh.
			 *
			 * Without this, ZBOSS spends time on a fruitless rejoin
			 * attempt on every boot before falling through to steering
			 * — burning through ZHA's permit-join window and making
			 * the device appear unfindable.
			 *
			 * zb_bdb_reset_via_local_action clears NVRAM and triggers
			 * a new DEVICE_FIRST_START on the next stack run. */
			LOG_WRN("Rejoin failed — clearing Zigbee NVRAM via BDB reset");
			ZB_SCHEDULE_APP_CALLBACK(zb_bdb_reset_via_local_action, 0);
		}
		break;

	case ZB_BDB_SIGNAL_STEERING:
		if (status == RET_OK) {
			/* Joined a network for the first time (or after loss).
			 * Use a fast poll interval for the 60-second window
			 * so ZHA can complete its attribute interview. */
			LOG_INF("Network steering OK — starting interview window");
			on_network_joined();
		} else {
			/* Steering attempt timed out — ZHA wasn't in permit-join
			 * mode or didn't hear us.  The default handler retries with
			 * exponential back-off; we just log here.
			 *
			 * Do NOT call user_input_indicate() here: it contains an
			 * internal ZB_ERROR_CHECK that resets the device if the
			 * scheduler queue is momentarily full. */
			LOG_WRN("Steering attempt failed — will retry");
		}
		break;

	case ZB_ZDO_SIGNAL_LEAVE:
		LOG_INF("Left Zigbee network");
		ZB_SCHEDULE_APP_ALARM_CANCEL(measure_and_report,
					     ZB_ALARM_ANY_PARAM);
		break;

	default:
		/* Log unhandled signals so we can see the full ZBOSS event stream */
		LOG_INF("Zigbee signal 0x%x status %d (unhandled)", (unsigned)sig, (int)status);
		break;
	}

	/* Call WITHOUT ZB_ERROR_CHECK.  If the default handler's internal
	 * callback queue is momentarily full it returns a non-RET_OK value,
	 * ZB_ERROR_CHECK would trigger a ZBOSS assert, and with
	 * ZBOSS_RESET_ON_ASSERT=y the device resets — creating an infinite
	 * steering-fail → reset loop.  Failures here are non-fatal; the
	 * handler will be called again on the next signal. */
	zigbee_default_signal_handler(bufid);

	/* CRITICAL: return the buffer to the ZBOSS pool after the default
	 * handler is done with it.  Without this, every Zigbee signal leaks
	 * one buffer.  After a handful of signals the pool is exhausted and
	 * bdb_start_top_level_commissioning() returns ZB_FALSE — silently
	 * refusing to start the channel scan — so ZB_BDB_SIGNAL_STEERING
	 * never fires and ZHA cannot discover the device. */
	if (bufid) {
		zb_buf_free(bufid);
	}
}

/* ── WDT keepalive ───────────────────────────────────────────────── */
/* The Adafruit bootloader starts a 1-second hardware WDT that cannot be
 * stopped.  Feed it every 500 ms from a kernel timer so the app never
 * accidentally resets during normal operation. */
static void wdt_keepalive_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	diag_pet_wdt();
}
static K_TIMER_DEFINE(wdt_keepalive_timer, wdt_keepalive_cb, NULL);

/* ── main ────────────────────────────────────────────────────────── */

int main(void)
{
	int err;

	/* Start WDT keepalive immediately — must feed within 1 s or reset */
	k_timer_start(&wdt_keepalive_timer, K_MSEC(500), K_MSEC(500));

	/* Diagnostic: turn LED off to signal main() was reached */
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, DIAG_LED_PIN));

	/* Give USB CDC time to enumerate so early log output is captured. */
	k_sleep(K_MSEC(500));
	LOG_INF("Sensor app starting");

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

	/* ── ADC: configure the VDD channel ── */
	err = adc_channel_setup_dt(&adc_vdd);
	if (err) {
		LOG_ERR("ADC channel setup: %d", err);
		return err;
	}
	adc_sequence_init_dt(&adc_vdd, &adc_seq);

	/* ── Zigbee: register endpoint ── */
	ZB_AF_REGISTER_DEVICE_CTX(&sensor_device_ctx);

	/* Enable sleepy end device behavior (radio off between polls).
	 * Must be called before zigbee_enable(). */
	zigbee_configure_sleepy_behavior(true);

	/* DEV: erase Zigbee NVRAM on every boot so the device always starts
	 * factory-new and goes straight to DEVICE_FIRST_START → steering.
	 * This avoids DEVICE_REBOOT attempting to rejoin a stale network and
	 * burning through ZHA's permit-join window before steering begins.
	 *
	 * NOTE: remove (or gate on a compile flag) before production — without
	 * this the device will re-pair on every reset instead of rejoining. */
	zigbee_erase_persistent_storage(ZB_TRUE);

	zigbee_enable();

	return 0;
}
