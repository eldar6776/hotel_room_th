#include "settings.h"
#include <Preferences.h>
#include "debug_logger.h"
#include "hvac.h"
#include "modbus_handler.h"
#include <lvgl.h>

// Forward declare ui_Main for screen-change use in inactivity callback
#ifdef __cplusplus
extern "C" {
#endif
#include "../lvgl/ui.h"
bool ui_clean_countdown_active(void);
#ifdef __cplusplus
}
#endif

// ── Globals ───────────────────────────────────────────────────────────────────
sys_config_t g_sys_cfg;
uint32_t     g_dirty_flags  = 0;
bool         g_wifi_ap_active = false;

static Preferences   s_prefs;
static bool          s_on_settings = false;
static unsigned long s_last_touch_ms = 0;
static bool          s_screensaver_active = false;

// ── Delayed-save state ────────────────────────────────────────────────────────
static bool          s_save_scheduled   = false;
static uint32_t s_save_started_ms = 0;

static bool timeout_is_valid(uint8_t t)
{
    return (t == 30) || (t == 60) || (t == 120);
}

static void inactivity_apply_screensaver(void)
{
    s_screensaver_active = true;
    if (ui_PinTextArea) lv_textarea_set_text(ui_PinTextArea, "");

    lv_obj_t *active = lv_scr_act();
    bool on_settings_screen =
        (active == ui_Settings1) ||
        (active == ui_Settings2) ||
        (active == ui_Settings3);

    if (on_settings_screen || s_on_settings) {
        settings_edit_cancel();
        s_on_settings = false;
    }

    // Dim display — g_sys_cfg.bright_low is now the NVS value if settings
    // were discarded above, or the committed value otherwise.
    hal_backlight_set(g_sys_cfg.bright_low);

    if (!g_wifi_ap_active) {
        setCpuFrequencyMhz(80);
        LOG_INFO("[CFG] CPU clock reduced to 80MHz (Screensaver)");
    } else {
        LOG_INFO("[CFG] CPU clock kept at 240MHz (WiFi AP active)");
    }

    if (active != ui_Main) {
        _ui_screen_change(&ui_Main, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_Main_screen_init);
    }
}

// ── NVS namespace ─────────────────────────────────────────────────────────────
#define NVS_NS "thermostat"

// ── settings_init ─────────────────────────────────────────────────────────────
void settings_init(void)
{
    // Defaults
    g_sys_cfg.temp_min          = 16;
    g_sys_cfg.temp_max          = 30;
    g_sys_cfg.target_temp       = 22;  // °C setpoint default
    g_sys_cfg.hvac_mode         = 0;
    g_sys_cfg.ctrl_type         = 0;
    g_sys_cfg.hysteresis_x10    = 10;   // 1.0 °C
    g_sys_cfg.stage_step_x10    = 10;   // 1.0 °C
    g_sys_cfg.sensor_offset_x10 = 0;
    g_sys_cfg.bright_high       = 900;
    g_sys_cfg.bright_low        = 300;
    g_sys_cfg.timeout_s         = INACTIVITY_TIMEOUT_DEFAULT_S;
    g_sys_cfg.modbus_addr       = 1;
    g_sys_cfg.theme_select      = 0;

    // Open NVS in read-write mode. This will create the namespace if it's the
    // first boot. Opening in read-only (true) would fail on a fresh device.
    s_prefs.begin(NVS_NS, false);
    g_sys_cfg.temp_min          = (int16_t) s_prefs.getInt("temp_min",           g_sys_cfg.temp_min);
    g_sys_cfg.temp_max          = (int16_t) s_prefs.getInt("temp_max",           g_sys_cfg.temp_max);
    g_sys_cfg.target_temp       = (int16_t) s_prefs.getInt("target_temp",        g_sys_cfg.target_temp);
    g_sys_cfg.hvac_mode         = (uint8_t) s_prefs.getUInt("hvac_mode",         g_sys_cfg.hvac_mode);
    g_sys_cfg.ctrl_type         = (uint8_t) s_prefs.getUInt("ctrl_type",         g_sys_cfg.ctrl_type);
    g_sys_cfg.hysteresis_x10    = (int16_t) s_prefs.getInt("hyst_x10",           g_sys_cfg.hysteresis_x10);
    g_sys_cfg.stage_step_x10    = (int16_t) s_prefs.getInt("stage_x10",          g_sys_cfg.stage_step_x10);
    g_sys_cfg.sensor_offset_x10 = settings_clamp_sensor_offset(s_prefs.getInt("offset_x10", g_sys_cfg.sensor_offset_x10));
    g_sys_cfg.bright_high       = (uint16_t)s_prefs.getUInt("bright_high",        g_sys_cfg.bright_high);
    g_sys_cfg.bright_low        = (uint16_t)s_prefs.getUInt("bright_low",         g_sys_cfg.bright_low);
    g_sys_cfg.timeout_s         = (uint8_t) s_prefs.getUInt("timeout_s",          g_sys_cfg.timeout_s);
    g_sys_cfg.modbus_addr       = (uint8_t) s_prefs.getUInt("modbus_addr",        g_sys_cfg.modbus_addr);
    g_sys_cfg.theme_select      = (uint8_t) s_prefs.getUChar("theme",             g_sys_cfg.theme_select);
    if (g_sys_cfg.theme_select > 1) g_sys_cfg.theme_select = 0;
    s_prefs.end();

    // Clamp modbus address
    if (g_sys_cfg.modbus_addr < 1 || g_sys_cfg.modbus_addr > 247)
        g_sys_cfg.modbus_addr = 1;

    // Clamp setpoint to valid range
    if (g_sys_cfg.target_temp < 10) g_sys_cfg.target_temp = 10;
    if (g_sys_cfg.target_temp > 40) g_sys_cfg.target_temp = 40;

    // Clamp inactivity timeout to supported UI values.
    if (!timeout_is_valid(g_sys_cfg.timeout_s)) {
        g_sys_cfg.timeout_s = INACTIVITY_TIMEOUT_DEFAULT_S;
    }

    LOG_INFO("[CFG] Loaded: modbus=%u  bright_h=%u  bright_l=%u  hyst=%.1f",
             g_sys_cfg.modbus_addr,
             g_sys_cfg.bright_high, g_sys_cfg.bright_low,
             g_sys_cfg.hysteresis_x10 / 10.0f);
}

// ── settings_save_dirty ───────────────────────────────────────────────────────
bool settings_save_dirty(void)
{
    if (g_dirty_flags == 0) return true;

    if (!s_prefs.begin(NVS_NS, false)) { settings_schedule_save(); return false; }
    uint32_t saved = 0;

    if ((g_dirty_flags & FLAG_TEMP_MIN) && s_prefs.putInt("temp_min",    g_sys_cfg.temp_min) != 0) saved |= FLAG_TEMP_MIN;
    if ((g_dirty_flags & FLAG_TEMP_MAX) && s_prefs.putInt("temp_max",    g_sys_cfg.temp_max) != 0) saved |= FLAG_TEMP_MAX;
    if ((g_dirty_flags & FLAG_TARGET_TEMP) && s_prefs.putInt("target_temp", g_sys_cfg.target_temp) != 0) saved |= FLAG_TARGET_TEMP;
    if ((g_dirty_flags & FLAG_HVAC_MODE) && s_prefs.putUInt("hvac_mode",  g_sys_cfg.hvac_mode) != 0) saved |= FLAG_HVAC_MODE;
    if ((g_dirty_flags & FLAG_CTRL_TYPE) && s_prefs.putUInt("ctrl_type",  g_sys_cfg.ctrl_type) != 0) saved |= FLAG_CTRL_TYPE;
    if ((g_dirty_flags & FLAG_HYSTERESIS) && s_prefs.putInt("hyst_x10",    g_sys_cfg.hysteresis_x10) != 0) saved |= FLAG_HYSTERESIS;
    if ((g_dirty_flags & FLAG_STAGE_STEP) && s_prefs.putInt("stage_x10",   g_sys_cfg.stage_step_x10) != 0) saved |= FLAG_STAGE_STEP;
    if ((g_dirty_flags & FLAG_SENSOR_OFFSET) && s_prefs.putInt("offset_x10",  g_sys_cfg.sensor_offset_x10) != 0) saved |= FLAG_SENSOR_OFFSET;
    if ((g_dirty_flags & FLAG_BRIGHT_HIGH) && s_prefs.putUInt("bright_high",g_sys_cfg.bright_high) != 0) saved |= FLAG_BRIGHT_HIGH;
    if ((g_dirty_flags & FLAG_BRIGHT_LOW) && s_prefs.putUInt("bright_low", g_sys_cfg.bright_low) != 0) saved |= FLAG_BRIGHT_LOW;
    if ((g_dirty_flags & FLAG_TIMEOUT) && s_prefs.putUInt("timeout_s",  g_sys_cfg.timeout_s) != 0) saved |= FLAG_TIMEOUT;
    if ((g_dirty_flags & FLAG_MODBUS_ADDR) && s_prefs.putUInt("modbus_addr",g_sys_cfg.modbus_addr) != 0) saved |= FLAG_MODBUS_ADDR;
    if ((g_dirty_flags & FLAG_THEME_SELECT) && s_prefs.putUChar("theme",     g_sys_cfg.theme_select) != 0) saved |= FLAG_THEME_SELECT;

    s_prefs.end();
    LOG_INFO("[CFG] Saved dirty flags: 0x%04X", g_dirty_flags);
    g_dirty_flags &= ~saved;

    // Sync newly saved NVS configurations to Modbus holding registers
    modbus_sync_from_settings();
    if (g_dirty_flags) settings_schedule_save();
    return g_dirty_flags == 0;
}

void settings_reset_dirty(void)
{
    g_dirty_flags      = 0;
    s_save_scheduled   = false;
}

// ── settings_schedule_save ────────────────────────────────────────────────────
// Schedule an NVS write 3 s after the last call.  Subsequent calls within the
// window simply push the deadline forward — only one commit per interaction.
void settings_schedule_save(void)
{
    s_save_scheduled   = true;
    s_save_started_ms = (uint32_t)millis();
}

// ── settings_tick ─────────────────────────────────────────────────────────────
// Must be called from the main loop.  Fires the deferred NVS write once the
// 3-second idle window has elapsed.
void settings_tick(void)
{
    if (s_save_scheduled && ((uint32_t)((uint32_t)millis() - s_save_started_ms) >= 3000U)) {
        s_save_scheduled = false;
        settings_save_dirty();
    }
}

// ── Inactivity timeout ────────────────────────────────────────────────────────
void inactivity_reset(void)
{
    if (s_screensaver_active) return;  // ignore events during screen transition
    
    if (getCpuFrequencyMhz() != 240) {
        setCpuFrequencyMhz(240);
        LOG_INFO("[CFG] CPU clock restored to 240MHz (Wakeup)");
    }
    
    // Keep legacy API behavior but ensure any touch restores high brightness.
    hal_backlight_set(s_on_settings ? settings_edit_config()->bright_high : g_sys_cfg.bright_high);
    s_last_touch_ms = millis();
}

void inactivity_touch_event(void)
{
    s_screensaver_active = false;  // real user touch clears the flag
    inactivity_reset();
}

void inactivity_set_on_settings(bool on)
{
    s_on_settings    = on;
    if (on) {
        settings_edit_begin();
        s_screensaver_active = false;
    }
    s_last_touch_ms  = millis();  // reset timer when entering/leaving settings
}

bool inactivity_on_settings_screen(void)
{
    return s_on_settings;
}

void inactivity_check(void)
{
    if (g_wifi_ap_active) return;   // paused while WiFi AP is active
    if (ui_clean_countdown_active()) return;
    if (s_screensaver_active) return;

    // Uklonjeno ograničenje: ranije se timeout primjenjivao samo na Settings ekranima.
    // Sada će se ekran dimmati na g_sys_cfg.bright_low nakon isteka vremena, 
    // bez obzira na kojem ekranu se nalazite.
    lv_obj_t *active = lv_scr_act();

    // Runtime safety net: if NVS is corrupted, fall back to default.
    if (!timeout_is_valid(g_sys_cfg.timeout_s)) {
        g_sys_cfg.timeout_s = INACTIVITY_TIMEOUT_DEFAULT_S;
    }

    unsigned long elapsed = millis() - s_last_touch_ms;
    if (elapsed >= ((unsigned long)g_sys_cfg.timeout_s * 1000UL)) {
        LOG_INFO("[CFG] Inactivity timeout -> screensaver (dim + Main)");
        inactivity_apply_screensaver();
        s_last_touch_ms = millis();
    }
}

void inactivity_force_timeout(void)
{
    inactivity_apply_screensaver();
    s_last_touch_ms = millis();
}

bool inactivity_is_screensaver_active(void)
{
    return s_screensaver_active;
}
