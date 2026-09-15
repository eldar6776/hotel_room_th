#include "settings.h"
static sys_config_t draft;
static uint32_t edited;
static bool editing;
sys_config_t *settings_edit_config(void) { return editing ? &draft : &g_sys_cfg; }
void settings_edit_begin(void) { if (!editing) { draft=g_sys_cfg; edited=0; editing=true; } }
void settings_edit_cancel(void) { editing=false; edited=0; }
void settings_edit_mark(uint32_t flag) {
    if (!editing) return;
    if (flag & FLAG_TEMP_MIN) { if (draft.temp_min != g_sys_cfg.temp_min) edited |= FLAG_TEMP_MIN; else edited &= ~FLAG_TEMP_MIN; }
    if (flag & FLAG_TEMP_MAX) { if (draft.temp_max != g_sys_cfg.temp_max) edited |= FLAG_TEMP_MAX; else edited &= ~FLAG_TEMP_MAX; }
    if (flag & FLAG_HVAC_MODE) { if (draft.hvac_mode != g_sys_cfg.hvac_mode) edited |= FLAG_HVAC_MODE; else edited &= ~FLAG_HVAC_MODE; }
    if (flag & FLAG_CTRL_TYPE) { if (draft.ctrl_type != g_sys_cfg.ctrl_type) edited |= FLAG_CTRL_TYPE; else edited &= ~FLAG_CTRL_TYPE; }
    if (flag & FLAG_HYSTERESIS) { if (draft.hysteresis_x10 != g_sys_cfg.hysteresis_x10) edited |= FLAG_HYSTERESIS; else edited &= ~FLAG_HYSTERESIS; }
    if (flag & FLAG_STAGE_STEP) { if (draft.stage_step_x10 != g_sys_cfg.stage_step_x10) edited |= FLAG_STAGE_STEP; else edited &= ~FLAG_STAGE_STEP; }
    if (flag & FLAG_SENSOR_OFFSET) { if (draft.sensor_offset_x10 != g_sys_cfg.sensor_offset_x10) edited |= FLAG_SENSOR_OFFSET; else edited &= ~FLAG_SENSOR_OFFSET; }
    if (flag & FLAG_BRIGHT_HIGH) { if (draft.bright_high != g_sys_cfg.bright_high) edited |= FLAG_BRIGHT_HIGH; else edited &= ~FLAG_BRIGHT_HIGH; }
    if (flag & FLAG_BRIGHT_LOW) { if (draft.bright_low != g_sys_cfg.bright_low) edited |= FLAG_BRIGHT_LOW; else edited &= ~FLAG_BRIGHT_LOW; }
    if (flag & FLAG_TIMEOUT) { if (draft.timeout_s != g_sys_cfg.timeout_s) edited |= FLAG_TIMEOUT; else edited &= ~FLAG_TIMEOUT; }
    if (flag & FLAG_MODBUS_ADDR) { if (draft.modbus_addr != g_sys_cfg.modbus_addr) edited |= FLAG_MODBUS_ADDR; else edited &= ~FLAG_MODBUS_ADDR; }
    if (flag & FLAG_THEME_SELECT) { if (draft.theme_select != g_sys_cfg.theme_select) edited |= FLAG_THEME_SELECT; else edited &= ~FLAG_THEME_SELECT; }
}
bool settings_edit_refresh(void) {
    if (!editing) return false;
    bool changed=false;
    if (!(edited & FLAG_TEMP_MIN) && draft.temp_min != g_sys_cfg.temp_min) { draft.temp_min=g_sys_cfg.temp_min; changed=true; }
    if (!(edited & FLAG_TEMP_MAX) && draft.temp_max != g_sys_cfg.temp_max) { draft.temp_max=g_sys_cfg.temp_max; changed=true; }
    if (!(edited & FLAG_HVAC_MODE) && draft.hvac_mode != g_sys_cfg.hvac_mode) { draft.hvac_mode=g_sys_cfg.hvac_mode; changed=true; }
    if (!(edited & FLAG_CTRL_TYPE) && draft.ctrl_type != g_sys_cfg.ctrl_type) { draft.ctrl_type=g_sys_cfg.ctrl_type; changed=true; }
    if (!(edited & FLAG_HYSTERESIS) && draft.hysteresis_x10 != g_sys_cfg.hysteresis_x10) { draft.hysteresis_x10=g_sys_cfg.hysteresis_x10; changed=true; }
    if (!(edited & FLAG_STAGE_STEP) && draft.stage_step_x10 != g_sys_cfg.stage_step_x10) { draft.stage_step_x10=g_sys_cfg.stage_step_x10; changed=true; }
    if (!(edited & FLAG_SENSOR_OFFSET) && draft.sensor_offset_x10 != g_sys_cfg.sensor_offset_x10) { draft.sensor_offset_x10=g_sys_cfg.sensor_offset_x10; changed=true; }
    if (!(edited & FLAG_BRIGHT_HIGH) && draft.bright_high != g_sys_cfg.bright_high) { draft.bright_high=g_sys_cfg.bright_high; changed=true; }
    if (!(edited & FLAG_BRIGHT_LOW) && draft.bright_low != g_sys_cfg.bright_low) { draft.bright_low=g_sys_cfg.bright_low; changed=true; }
    if (!(edited & FLAG_TIMEOUT) && draft.timeout_s != g_sys_cfg.timeout_s) { draft.timeout_s=g_sys_cfg.timeout_s; changed=true; }
    if (!(edited & FLAG_MODBUS_ADDR) && draft.modbus_addr != g_sys_cfg.modbus_addr) { draft.modbus_addr=g_sys_cfg.modbus_addr; changed=true; }
    if (!(edited & FLAG_THEME_SELECT) && draft.theme_select != g_sys_cfg.theme_select) { draft.theme_select=g_sys_cfg.theme_select; changed=true; }
    return changed;
}
bool settings_edit_save(void) {
    if (!editing) return settings_save_dirty();
    if (edited & FLAG_TEMP_MIN) g_sys_cfg.temp_min=draft.temp_min;
    if (edited & FLAG_TEMP_MAX) g_sys_cfg.temp_max=draft.temp_max;
    if (edited & FLAG_HVAC_MODE) g_sys_cfg.hvac_mode=draft.hvac_mode;
    if (edited & FLAG_CTRL_TYPE) g_sys_cfg.ctrl_type=draft.ctrl_type;
    if (edited & FLAG_HYSTERESIS) g_sys_cfg.hysteresis_x10=draft.hysteresis_x10;
    if (edited & FLAG_STAGE_STEP) g_sys_cfg.stage_step_x10=draft.stage_step_x10;
    if (edited & FLAG_SENSOR_OFFSET) g_sys_cfg.sensor_offset_x10=draft.sensor_offset_x10;
    if (edited & FLAG_BRIGHT_HIGH) g_sys_cfg.bright_high=draft.bright_high;
    if (edited & FLAG_BRIGHT_LOW) g_sys_cfg.bright_low=draft.bright_low;
    if (edited & FLAG_TIMEOUT) g_sys_cfg.timeout_s=draft.timeout_s;
    if (edited & FLAG_MODBUS_ADDR) g_sys_cfg.modbus_addr=draft.modbus_addr;
    if (edited & FLAG_THEME_SELECT) g_sys_cfg.theme_select=draft.theme_select;
    g_dirty_flags |= edited;
    if (g_sys_cfg.target_temp < g_sys_cfg.temp_min) { g_sys_cfg.target_temp=g_sys_cfg.temp_min; g_dirty_flags |= FLAG_TARGET_TEMP; }
    if (g_sys_cfg.target_temp > g_sys_cfg.temp_max) { g_sys_cfg.target_temp=g_sys_cfg.temp_max; g_dirty_flags |= FLAG_TARGET_TEMP; }
    if (!settings_save_dirty()) return false;
    settings_edit_cancel();
    return true;
}
