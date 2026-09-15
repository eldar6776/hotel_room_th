#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "hal.h"
#include "settings_limits.h"

// ── Dirty flag bitmask (FSD §5.1) ────────────────────────────────────────────
typedef enum {
    FLAG_TEMP_MIN      = (1 <<  0),
    FLAG_TEMP_MAX      = (1 <<  1),
    FLAG_HVAC_MODE     = (1 <<  2),
    FLAG_CTRL_TYPE     = (1 <<  3),
    FLAG_HYSTERESIS    = (1 <<  4),
    FLAG_STAGE_STEP    = (1 <<  5),
    FLAG_SENSOR_OFFSET = (1 <<  6),
    FLAG_BRIGHT_HIGH   = (1 <<  7),
    FLAG_BRIGHT_LOW    = (1 <<  8),
    FLAG_TIMEOUT       = (1 <<  9),
    FLAG_MODBUS_ADDR   = (1 << 10),
    FLAG_TARGET_TEMP   = (1 << 11),
    FLAG_THEME_SELECT  = (1 << 12)
} settings_flag_t;

// ── System configuration RAM structure ───────────────────────────────────────
typedef struct {
    int16_t  temp_min;           // °C (default 16)
    int16_t  temp_max;           // °C (default 30)
    int16_t  target_temp;        // °C setpoint (default 22)
    uint8_t  hvac_mode;          // 0=OFF,1=HEAT,2=COOL
    uint8_t  ctrl_type;          // 0=Thermostat,1=Manual
    int16_t  hysteresis_x10;     // ×10 e.g. 10 = 1.0 °C
    int16_t  stage_step_x10;     // ×10
    int16_t  sensor_offset_x10;  // ×10 calibration offset
    uint16_t bright_high;        // backlight 0-1023 (day)
    uint16_t bright_low;         // backlight 0-1023 (night)
    uint8_t  timeout_s;          // inactivity timeout (30 s default)
    uint8_t  modbus_addr;        // 1-247
    uint8_t  theme_select;       // 0=NONE, 1=AURORA, 2=LOGO
} sys_config_t;

// ── Inactivity timeout ────────────────────────────────────────────────────────
#define INACTIVITY_TIMEOUT_DEFAULT_S   30
#define INACTIVITY_CHECK_INTERVAL_MS   1000

extern sys_config_t  g_sys_cfg;
extern uint32_t      g_dirty_flags;
extern bool          g_wifi_ap_active;

// ── Public API ────────────────────────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif
sys_config_t *settings_edit_config(void);
void settings_edit_begin(void);
void settings_edit_cancel(void);
void settings_edit_mark(uint32_t flag);
bool settings_edit_refresh(void);
bool settings_edit_save(void);
void settings_init(void);               // load from NVS
bool settings_save_dirty(void);         // write only dirty fields → NVS (immediate)
void settings_schedule_save(void);      // schedule NVS write 3 s after last call
void settings_tick(void);               // call from main loop — fires deferred save
void settings_reset_dirty(void);

void inactivity_reset(void);            // call on any touch event
void inactivity_touch_event(void);      // reset inactivity + restore high brightness
void inactivity_check(void);            // call from timer or loop
void inactivity_force_timeout(void);
bool inactivity_is_screensaver_active(void);
bool inactivity_on_settings_screen(void);
void inactivity_set_on_settings(bool on);
#ifdef __cplusplus
}
#endif
