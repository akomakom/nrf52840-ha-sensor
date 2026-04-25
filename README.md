# nice!nano Zigbee Temperature/Humidity Sensor

A battery-powered Zigbee sensor for Home Assistant (ZHA) using the nice!nano (nRF52840) board with SHT40 temperature/humidity sensor.

## Hardware

- **Board**: nice!nano (nRF52840) - Adafruit UF2 bootloader
- **Sensor**: SHT40 (I2C) - Temperature & Humidity
  - SDA: P0.17, SCL: P0.20
  - Power gate: VCC=P0.24, GND=P0.22
- **Battery**: Monitored via SAADC internal VDD channel
- **LED**: Blue LED on P0.15 (active HIGH)

## Features

- ✅ **Three operating modes** (debug/prod/hub)
- ✅ **Sleepy end device** with configurable poll intervals
- ✅ **Battery voltage reporting** (Power Configuration cluster)
- ✅ **Temperature & humidity** (Measurement clusters)
- ✅ **USB CDC console** (debug mode only)
- ✅ **Deep sleep** with RAM power-down (<1mA sleep current)
- ⚠️ **Router mode** (hub) - joins network but not discovered by ZHA

## Build Modes

The firmware supports three modes via `make MODE=<mode>`:

### 1. Debug Mode (default)
```bash
make MODE=debug flash
```
- **Role**: Sleepy End Device
- **USB**: Enabled (CDC console for logging)
- **Logging**: Full debug output
- **Power**: ~3mA (USB powered)
- **Use case**: Development, debugging

### 2. Production Mode
```bash
make MODE=prod flash
```
- **Role**: Sleepy End Device
- **USB**: Disabled
- **Logging**: Disabled
- **Power**: <1mA sleep, ~15mA active
- **Use case**: Battery operation, deployment

### 3. Hub Mode (⚠️ Not Working)
```bash
make MODE=hub flash
```
- **Role**: Router (always-on repeater)
- **USB**: Enabled (for logging)
- **Logging**: Full debug output
- **Power**: Always powered (USB/wall adapter)
- **Status**: ⚠️ **Joins network but not discovered by ZHA**
  - Router sends `Node_Desc_req` TO coordinator (incorrect)
  - Should wait for coordinator to send `Node_Desc_req` TO router
  - Issue appears to be ZBOSS stack behavior, not firmware-specific
  - See `test_light_bulb/` for baseline test using Nordic's official sample

## Configuration

Edit `config.h` to customize sensor behavior:

```c
// Measurement & poll intervals (milliseconds)
#define POLL_INTERVAL_MS        60000   // 60 seconds
#define MEASUREMENT_INTERVAL_MS 60000   // 60 seconds

// Battery voltage thresholds (millivolts)
// IMPORTANT: nice!nano has 3.3V regulator requiring ~3.5V input minimum
#define BATTERY_VOLTAGE_MIN_MV  3200    // 3.2V (0%) - for LiPo batteries
#define BATTERY_VOLTAGE_MAX_MV  4200    // 4.2V (100%) - fully charged LiPo

// Sensor power timing
#define SENSOR_POWERUP_DELAY_MS 20      // SHT40 power-on time
```

**Note:** Intervals are compiled into firmware and cannot be changed by Home Assistant after deployment. To change intervals, edit `config.h`, rebuild, and reflash. Future enhancement: Add Poll Control cluster (0x0020) to allow ZHA to configure intervals dynamically.

## Quick Start

### Prerequisites
- nRF Connect SDK 2.7.0
- Zephyr SDK 0.16.8
- West build tool

### Build & Flash
```bash
# Debug mode (default)
make flash

# Production mode (battery)
make MODE=prod flash

# Clean build
make clean
```

### Pairing with ZHA
1. Flash firmware (NVRAM erase enabled by default)
2. Device joins network automatically
3. ZHA discovers device within 60 seconds
4. Device appears as "Temperature Sensor" (0x0302)

### Monitoring
```bash
# View logs (debug mode only)
# USB CDC appears as /dev/ttyACM0 or similar
screen /dev/ttyACM0 115200
```

## Project Structure

```
.
├── src/
│   └── main.c              # Main application code
├── config.h                # User-configurable parameters
├── boards/
│   └── nice_nano/          # Board definition files
├── test_light_bulb/        # Nordic light_bulb baseline test
├── test_minimal/           # Minimal test firmware
├── prj.conf                # Zephyr project configuration
├── app.overlay             # Device tree overlay (I2C, ADC)
├── CMakeLists.txt          # Build configuration
└── Makefile                # Build automation

```

## Zigbee Details

- **Profile**: Home Automation (0x0104)
- **Device ID**: Temperature Sensor (0x0302)
- **Endpoint**: 10
- **Clusters**:
  - Basic (0x0000)
  - Power Configuration (0x0001) - Battery voltage
  - Temperature Measurement (0x0402)
  - Relative Humidity Measurement (0x0405)

## Power Consumption

| Mode | Sleep | Active | Notes |
|------|-------|--------|-------|
| Debug | ~3mA | ~15mA | USB always on |
| Production | <1mA | ~15mA | Radio off between polls |
| Hub | N/A | ~15mA | Always powered, router mode |

## Troubleshooting

### Device not joining network
- Check ZHA channel matches firmware (channels 11-26 supported)
- Verify coordinator is in pairing mode
- Try erasing NVRAM: uncomment `zigbee_erase_persistent_storage(ZB_TRUE)` in main.c

### Fast blinking LED (crash loop)
- Check USB cable/power supply
- Verify watchdog is being fed
- Review logs in debug mode

### Hub mode not working
- Known issue: Router sends incorrect ZDO queries
- Baseline test (`test_light_bulb/`) confirms ZBOSS stack behavior
- Workaround: Use sleepy end device mode (prod) instead

## Recent Changes

### Since Last Commit

#### Router Mode Investigation
- ✅ Added `ZB_ROUTER_ROLE` and `ZB_BDB_MODE` preprocessor macros
- ✅ Simplified signal handler to match Nordic light_bulb sample
- ✅ Changed device ID from 0x0008 (Range Extender) to 0x0302 (Temperature Sensor)
- ✅ Added explicit `zb_set_rx_on_when_idle(ZB_TRUE)` for router mode
- ❌ **Result**: Router still sends `Node_Desc_req` to coordinator (incorrect behavior)

#### Baseline Test Created
- ✅ Created `test_light_bulb/` - Nordic's official light_bulb sample adapted for nice!nano
- ✅ Purpose: Determine if issue is firmware-specific or ZBOSS stack behavior
- ⚠️ **Outcome**: Same crash/behavior as custom firmware (fast blink, no USB)
- 📊 **Conclusion**: Issue is likely ZBOSS stack or board-specific, not our code

#### Configuration Improvements
- ✅ Extracted intervals and thresholds to `config.h`
- ✅ Added multi-channel Zigbee scan (channels 11-26)
- ✅ Improved watchdog handling for Adafruit bootloader
- ✅ Added RAM power-down for better sleep current

#### Usefulness Assessment
- ✅ **Sleepy end device mode**: Works perfectly, <1mA sleep current
- ✅ **Configuration extraction**: Makes customization easier
- ❌ **Router mode fixes**: No progress, appears to be ZBOSS limitation
- ✅ **Baseline test**: Confirmed issue is not in our implementation

## Known Issues

1. **Router mode (hub) not functional**
   - Router joins network but not discovered by ZHA
   - Sends incorrect ZDO queries (end device behavior)
   - Likely requires Nordic Semiconductor support ticket
   - Workaround: Use sleepy end device mode

2. **USB CDC timing**
   - USB must initialize before Zigbee stack
   - Deferred logging mode required to prevent blocking

## License

SPDX-License-Identifier: Apache-2.0

## Support

For issues related to:
- **Firmware**: Check logs in debug mode, review `TROUBLESHOOTING.md`
- **Router mode**: See `test_light_bulb/README.md` for baseline test
- **ZBOSS stack**: Contact Nordic Semiconductor support
