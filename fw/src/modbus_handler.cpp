#include "modbus_handler.h"
#include "settings.h"
#include <sys/time.h>
#include <math.h>
#include "debug_logger.h"
#include "hal.h"
#include "hvac.h"  // for hvac_get_room_temp(), hvac_relay_active()
#include <ModbusRTU.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "version.h"

// ── Global data ───────────────────────────────────────────────────────────────
mb_data_t g_mb;

static ModbusRTU s_mb;

// Coil mirror — readable from main.cpp via extern
bool g_mb_coil_dnd = false;
bool g_mb_coil_mur = false;

// Outside Temperature tracking
static bool s_has_outside_temp = false;
static unsigned long s_last_outside_temp_ms = 0;
#define OUTSIDE_TEMP_TIMEOUT_MS 10800000UL // 3 hours

// ── Callbacks ────────────────────────────────────────────────────────────────

// Called by the library on every coil write from the master.
static uint16_t cb_coil_write(TRegister *reg, uint16_t val)
{
    uint16_t addr = reg->address.address;
    bool state = (val != 0);
    if (addr == MB_COIL_DND) {
        g_mb_coil_dnd = state;
        if (state) {
            g_mb_coil_mur = false;
            s_mb.Coil(MB_COIL_MUR, false);  // sync library register — master reads correct state
        }
    }
    if (addr == MB_COIL_MUR) {
        g_mb_coil_mur = state;
        if (state) {
            g_mb_coil_dnd = false;
            s_mb.Coil(MB_COIL_DND, false);  // sync library register — master reads correct state
        }
    }
    return val;
}

// Called by the library on every holding-register write from the master.
// Signature: uint16_t cb(TRegister* reg, uint16_t val)
// Return val to accept it unchanged, or return a different value to override.
static uint16_t cb_hreg_write(TRegister *reg, uint16_t val)
{
    uint16_t reg_index = reg->address.address;
    
    // Safety check against Buffer Overflow
    if (reg_index >= MB_HREG_COUNT) return val;
    
    switch (reg_index) {
        case MB_REG_TARGET_TEMP: {
            val = constrain(val, g_sys_cfg.temp_min * 10, g_sys_cfg.temp_max * 10);
            int16_t target_c = (int16_t)(val / 10);
            if (g_sys_cfg.target_temp != target_c) {
                g_sys_cfg.target_temp = target_c;
                g_dirty_flags |= FLAG_TARGET_TEMP;
                settings_schedule_save();
            }
            break;
        }
        case MB_REG_HVAC_MODE:
            if (val > HVAC_COOL) val = HVAC_OFF;
            if (g_sys_cfg.hvac_mode != val) {
                g_sys_cfg.hvac_mode = (uint8_t)val;
                g_dirty_flags |= FLAG_HVAC_MODE;
                settings_schedule_save();
            }
            break;
        case MB_REG_FAN_SPEED:
            if (val > FAN_HIGH) val = FAN_AUTO;
            break;
        case MB_REG_RELAY_MODE:
            if (val > 1) val = 0;
            if (g_sys_cfg.ctrl_type != val) {
                g_sys_cfg.ctrl_type = (uint8_t)val;
                g_dirty_flags |= FLAG_CTRL_TYPE;
                settings_schedule_save();
            }
            break;
        case MB_REG_OUTSIDE_TEMP:
            // Vanjska temperatura ažurirana
            s_has_outside_temp = true;
            s_last_outside_temp_ms = millis();
            break;
        case MB_REG_UNIX_TIME_L:
            // Master should write H and L together. We act on H write.
            break;
        case MB_REG_UNIX_TIME_H: {
            uint32_t unix_time = ((uint32_t)val << 16) | g_mb.hreg[MB_REG_UNIX_TIME_L];
            timeval tv = { .tv_sec = static_cast<time_t>(unix_time), .tv_usec = 0 };
            settimeofday(&tv, NULL);
            on_time_synced("Modbus");
            LOG_INFO("[MB] Time synced from Modbus: %lu", unix_time);
            break;
        }
        case MB_REG_TEMP_MIN:
            val = constrain(val, 10, 24);
            if (g_sys_cfg.temp_min != (int16_t)val) {
                g_sys_cfg.temp_min = (int16_t)val;
                g_dirty_flags |= FLAG_TEMP_MIN;
                settings_schedule_save();
            }
            break;
        case MB_REG_TEMP_MAX:
            val = constrain(val, 25, 40);
            if (g_sys_cfg.temp_max != (int16_t)val) {
                g_sys_cfg.temp_max = (int16_t)val;
                g_dirty_flags |= FLAG_TEMP_MAX;
                settings_schedule_save();
            }
            break;
        case MB_REG_HYSTERESIS:
            val = constrain(val, 2, 20);
            if (g_sys_cfg.hysteresis_x10 != (int16_t)val) {
                g_sys_cfg.hysteresis_x10 = (int16_t)val;
                g_dirty_flags |= FLAG_HYSTERESIS;
                settings_schedule_save();
            }
            break;
        case MB_REG_STAGE_STEP:
            val = constrain(val, 5, 25);
            if (g_sys_cfg.stage_step_x10 != (int16_t)val) {
                g_sys_cfg.stage_step_x10 = (int16_t)val;
                g_dirty_flags |= FLAG_STAGE_STEP;
                settings_schedule_save();
            }
            break;
        case MB_REG_SENSOR_OFFSET:
            val = (uint16_t)settings_clamp_sensor_offset((int16_t)val);
            if (g_sys_cfg.sensor_offset_x10 != (int16_t)val) {
                g_sys_cfg.sensor_offset_x10 = (int16_t)val;
                g_dirty_flags |= FLAG_SENSOR_OFFSET;
                settings_schedule_save();
            }
            break;
        case MB_REG_BRIGHT_HIGH:
            val = constrain(val, 0, 1023);
            if (g_sys_cfg.bright_high != val) {
                g_sys_cfg.bright_high = val;
                g_dirty_flags |= FLAG_BRIGHT_HIGH;
                settings_schedule_save();
            }
            break;
        case MB_REG_BRIGHT_LOW:
            val = constrain(val, 0, 1023);
            if (g_sys_cfg.bright_low != val) {
                g_sys_cfg.bright_low = val;
                g_dirty_flags |= FLAG_BRIGHT_LOW;
                settings_schedule_save();
            }
            break;
        case MB_REG_TIMEOUT_S:
            if (val != 30 && val != 60 && val != 120) {
                val = 30; // fallback to default
            }
            if (g_sys_cfg.timeout_s != val) {
                g_sys_cfg.timeout_s = (uint8_t)val;
                g_dirty_flags |= FLAG_TIMEOUT;
                settings_schedule_save();
            }
            break;
        case MB_REG_THEME_SELECT:
            if (val > 1) val = 0;
            if (g_sys_cfg.theme_select != val) {
                g_sys_cfg.theme_select = (uint8_t)val;
                g_dirty_flags |= FLAG_THEME_SELECT;
                settings_schedule_save();
            }
            break;
        default:
            break;
    }
    
    g_mb.hreg[reg_index] = val;
    return val;
}

// ── modbus_init ───────────────────────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif

void modbus_init(uint8_t slave_addr)
{
    memset(&g_mb, 0, sizeof(g_mb));

    // Modbus shares Serial (UART0) with debug — same baud rate 115200
    // RS485 DE pin: IO41 (dedicated pin, no SPI sharing)
    s_mb.begin(&Serial, PIN_RS485_DE);
    s_mb.slave(slave_addr);

    // Register holding registers block (0-based, MB_HREG_COUNT regs)
    s_mb.addHreg(0, 0, MB_HREG_COUNT);

    // Register input registers (read-only, 30001+)
    s_mb.addIreg(0, 0, MB_IREG_COUNT);

    // Initialize read-only version registers
    g_mb.ireg[MB_IREG_VERSION_MAJOR] = VERSION_MAJOR;
    g_mb.ireg[MB_IREG_VERSION_MINOR] = VERSION_MINOR;
    g_mb.ireg[MB_IREG_VERSION_PATCH] = VERSION_PATCH;
    s_mb.Ireg(MB_IREG_VERSION_MAJOR, VERSION_MAJOR);
    s_mb.Ireg(MB_IREG_VERSION_MINOR, VERSION_MINOR);
    s_mb.Ireg(MB_IREG_VERSION_PATCH, VERSION_PATCH);

    // Register discrete inputs (read-only bits, 10001+)
    s_mb.addIsts(0, false, MB_ISTS_COUNT);

    // Register coils (DND/MUR)
    s_mb.addCoil(MB_COIL_DND, false, 2);

    // Register coil write callback so GUI stays in sync when master writes
    s_mb.onSetCoil(MB_COIL_DND, cb_coil_write, 2);

    // Pre-load default values
    // Load values from NVS settings where available, otherwise use defaults.
    // This ensures that settings persist after a restart.
    g_mb.hreg[MB_REG_FAN_SPEED]   = FAN_AUTO; // Default fan speed
    s_mb.Hreg(MB_REG_FAN_SPEED, FAN_AUTO);
    modbus_sync_from_settings();

    // Register write callbacks so we react to master writes
    s_mb.onSetHreg(0, cb_hreg_write, MB_HREG_COUNT);

    LOG_INFO("[MB]  Slave init, addr=%u (shared with Serial @115200)", slave_addr);
}

// ── modbus_poll ───────────────────────────────────────────────────────────────
void modbus_poll(void)
{
    s_mb.task();
}

// ── Helpers ───────────────────────────────────────────────────────────────────
void modbus_set_target_temp(uint16_t val_x10)
{
    g_mb.hreg[MB_REG_TARGET_TEMP] = val_x10;
    s_mb.Hreg(MB_REG_TARGET_TEMP, val_x10);
}

void modbus_set_hvac_mode(uint8_t mode)
{
    g_mb.hreg[MB_REG_HVAC_MODE] = mode;
    s_mb.Hreg(MB_REG_HVAC_MODE, mode);
}

void modbus_set_relay_mode(uint8_t mode)
{
    g_mb.hreg[MB_REG_RELAY_MODE] = mode;
    s_mb.Hreg(MB_REG_RELAY_MODE, mode);
}

void modbus_set_fan_speed(uint8_t speed)
{
    LOG_INFO("[MB] modbus_set_fan_speed(%u) called", speed);
    g_mb.hreg[MB_REG_FAN_SPEED] = speed;
    s_mb.Hreg(MB_REG_FAN_SPEED, speed);
    LOG_INFO("[MB] After set: g_mb.hreg[%d]=%u, s_mb.Hreg(%d)=%u", 
             MB_REG_FAN_SPEED, g_mb.hreg[MB_REG_FAN_SPEED], 
             MB_REG_FAN_SPEED, s_mb.Hreg(MB_REG_FAN_SPEED));
}

void modbus_set_dnd_coil(bool state)
{
    g_mb_coil_dnd = state;
    s_mb.Coil(MB_COIL_DND, state);
}

void modbus_set_mur_coil(bool state)
{
    g_mb_coil_mur = state;
    s_mb.Coil(MB_COIL_MUR, state);
}

void modbus_set_slave_addr(uint8_t addr)
{
    if (addr < 1 || addr > 247) return;
    s_mb.slave(addr);
    LOG_INFO("[MB] Slave address changed to %u", addr);
}

void modbus_sync_from_settings(void)
{
    g_mb.hreg[MB_REG_TARGET_TEMP]   = (uint16_t)(g_sys_cfg.target_temp * 10);
    g_mb.hreg[MB_REG_HVAC_MODE]     = g_sys_cfg.hvac_mode;
    g_mb.hreg[MB_REG_RELAY_MODE]    = g_sys_cfg.ctrl_type;
    g_mb.hreg[MB_REG_TEMP_MIN]      = g_sys_cfg.temp_min;
    g_mb.hreg[MB_REG_TEMP_MAX]      = g_sys_cfg.temp_max;
    g_mb.hreg[MB_REG_HYSTERESIS]    = g_sys_cfg.hysteresis_x10;
    g_mb.hreg[MB_REG_STAGE_STEP]    = g_sys_cfg.stage_step_x10;
    g_mb.hreg[MB_REG_SENSOR_OFFSET] = g_sys_cfg.sensor_offset_x10;
    g_mb.hreg[MB_REG_BRIGHT_HIGH]   = g_sys_cfg.bright_high;
    g_mb.hreg[MB_REG_BRIGHT_LOW]    = g_sys_cfg.bright_low;
    g_mb.hreg[MB_REG_TIMEOUT_S]     = g_sys_cfg.timeout_s;
    g_mb.hreg[MB_REG_THEME_SELECT]  = g_sys_cfg.theme_select;

    s_mb.Hreg(MB_REG_TARGET_TEMP,   g_mb.hreg[MB_REG_TARGET_TEMP]);
    s_mb.Hreg(MB_REG_HVAC_MODE,     g_mb.hreg[MB_REG_HVAC_MODE]);
    s_mb.Hreg(MB_REG_RELAY_MODE,    g_mb.hreg[MB_REG_RELAY_MODE]);
    s_mb.Hreg(MB_REG_TEMP_MIN,      g_mb.hreg[MB_REG_TEMP_MIN]);
    s_mb.Hreg(MB_REG_TEMP_MAX,      g_mb.hreg[MB_REG_TEMP_MAX]);
    s_mb.Hreg(MB_REG_HYSTERESIS,    g_mb.hreg[MB_REG_HYSTERESIS]);
    s_mb.Hreg(MB_REG_STAGE_STEP,    g_mb.hreg[MB_REG_STAGE_STEP]);
    s_mb.Hreg(MB_REG_SENSOR_OFFSET, g_mb.hreg[MB_REG_SENSOR_OFFSET]);
    s_mb.Hreg(MB_REG_BRIGHT_HIGH,   g_mb.hreg[MB_REG_BRIGHT_HIGH]);
    s_mb.Hreg(MB_REG_BRIGHT_LOW,    g_mb.hreg[MB_REG_BRIGHT_LOW]);
    s_mb.Hreg(MB_REG_TIMEOUT_S,     g_mb.hreg[MB_REG_TIMEOUT_S]);
    s_mb.Hreg(MB_REG_THEME_SELECT,  g_mb.hreg[MB_REG_THEME_SELECT]);
}

// ── modbus_update_inputs ──────────────────────────────────────────────────────
// Update all input registers and discrete inputs from current system state.
// Call this periodically (e.g., from main loop every 500ms).
void modbus_update_inputs(void)
{
    // Input Register 0: Current temperature x10
    float temp_c = hvac_get_room_temp() + g_sys_cfg.sensor_offset_x10 / 10.0f;
    g_mb.ireg[MB_IREG_CURRENT_TEMP] = (uint16_t)(int16_t)lroundf(temp_c * 10.0f);
    s_mb.Ireg(MB_IREG_CURRENT_TEMP, g_mb.ireg[MB_IREG_CURRENT_TEMP]);

    // Input Register 1: Target temperature x10 (mirror from holding reg)
    g_mb.ireg[MB_IREG_TARGET_TEMP] = g_mb.hreg[MB_REG_TARGET_TEMP];
    s_mb.Ireg(MB_IREG_TARGET_TEMP, g_mb.ireg[MB_IREG_TARGET_TEMP]);

    // Input Register 2: Relay status (bit field) — read via I2C expander adapter
    uint16_t relay_bits = 0;
    if (hal_relay_is_on(1)) relay_bits |= (1 << 0);
    if (hal_relay_is_on(2)) relay_bits |= (1 << 1);
    if (hal_relay_is_on(3)) relay_bits |= (1 << 2);
    g_mb.ireg[MB_IREG_RELAY_STATUS] = relay_bits;
    s_mb.Ireg(MB_IREG_RELAY_STATUS, relay_bits);

    // Input Register 3-4: Uptime in seconds (32-bit split)
    uint32_t uptime_sec = millis() / 1000;
    g_mb.ireg[MB_IREG_UPTIME_LOW]  = (uint16_t)(uptime_sec & 0xFFFF);
    g_mb.ireg[MB_IREG_UPTIME_HIGH] = (uint16_t)(uptime_sec >> 16);
    s_mb.Ireg(MB_IREG_UPTIME_LOW, g_mb.ireg[MB_IREG_UPTIME_LOW]);
    s_mb.Ireg(MB_IREG_UPTIME_HIGH, g_mb.ireg[MB_IREG_UPTIME_HIGH]);

    // Input Register 5: Free heap in KB
    uint16_t free_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    g_mb.ireg[MB_IREG_FREE_HEAP] = free_kb;
    s_mb.Ireg(MB_IREG_FREE_HEAP, free_kb);

    // Input Register 6: Window sensor raw (0=open, 1=closed)
    bool window_closed = !hvac_is_window_open();
    g_mb.ireg[MB_IREG_WINDOW_RAW] = window_closed ? 1 : 0;
    s_mb.Ireg(MB_IREG_WINDOW_RAW, g_mb.ireg[MB_IREG_WINDOW_RAW]);

    // Input Register 7: Temperature sensor fault (1=fault, 0=ok)
    bool sensor_fault = hvac_temp_sensor_fault();
    g_mb.ireg[MB_IREG_SENSOR_FAULT] = sensor_fault ? 1 : 0;
    s_mb.Ireg(MB_IREG_SENSOR_FAULT, g_mb.ireg[MB_IREG_SENSOR_FAULT]);

    // Input Registers 8-10: Firmware version verification
    g_mb.ireg[MB_IREG_VERSION_MAJOR] = VERSION_MAJOR;
    g_mb.ireg[MB_IREG_VERSION_MINOR] = VERSION_MINOR;
    g_mb.ireg[MB_IREG_VERSION_PATCH] = VERSION_PATCH;
    s_mb.Ireg(MB_IREG_VERSION_MAJOR, VERSION_MAJOR);
    s_mb.Ireg(MB_IREG_VERSION_MINOR, VERSION_MINOR);
    s_mb.Ireg(MB_IREG_VERSION_PATCH, VERSION_PATCH);

    // Discrete Input 0: Window closed
    s_mb.Ists(MB_ISTS_WINDOW_CLOSED, window_closed);

    // Discrete Input 1: System ready (always true after init)
    s_mb.Ists(MB_ISTS_SYSTEM_READY, true);

    // Discrete Input 2: HVAC active (any relay on)
    bool hvac_active = (relay_bits != 0);
    s_mb.Ists(MB_ISTS_HVAC_ACTIVE, hvac_active);

    // Discrete Input 3: Temperature sensor fault alarm
    s_mb.Ists(MB_ISTS_SENSOR_FAULT, sensor_fault);
}

// ── Getter functions ──────────────────────────────────────────────────────────
uint16_t modbus_get_current_temp(void)
{
    return g_mb.ireg[MB_IREG_CURRENT_TEMP];
}

uint16_t modbus_get_relay_status(void)
{
    return g_mb.ireg[MB_IREG_RELAY_STATUS];
}

bool modbus_get_window_closed(void)
{
    return (g_mb.ireg[MB_IREG_WINDOW_RAW] != 0);
}

bool modbus_get_hvac_active(void)
{
    return s_mb.Ists(MB_ISTS_HVAC_ACTIVE);
}

bool modbus_has_outside_temp(void)
{
    return s_has_outside_temp;
}

void modbus_check_outside_temp_timeout(void)
{
    if (s_has_outside_temp) {
        if (millis() - s_last_outside_temp_ms >= OUTSIDE_TEMP_TIMEOUT_MS) {
            s_has_outside_temp = false;
            LOG_INFO("[MB] Outside temperature timed out (> 3 hours)");
        }
    }
}

#ifdef __cplusplus
}
#endif
