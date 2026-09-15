// Custom UI event implementations — safe from SquareLine Studio overwrites
// Original content from lvgl/ui_events.c, moved here for persistence

#include <lvgl.h>
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "hal.h"
#include "hvac.h"
#include "modbus_handler.h"
#include "settings.h"
#include "wifi_manager.h"
#include "debug_logger_c.h"
#include "ui_events.h"

// Forward declarations for SquareLine-generated objects
extern lv_obj_t *ui_SwipeContainer;
extern lv_obj_t *ui_LabelCleanCountdown;
extern lv_obj_t *ui_LabelCleanText;
extern lv_obj_t *ui_LabelCleanMsg;
extern lv_obj_t *ui_TileThermostat;
extern lv_obj_t *ui_ArcTemp;
extern lv_obj_t *ui_LabelTargetTemp;
extern lv_obj_t *ui_LabelFanStatus;
extern lv_obj_t *ui_LabelCurrentTemp;
extern lv_obj_t *ui_LabelRoomTemp;
extern lv_obj_t *ui_ImageHeatStatus;
extern lv_obj_t *ui_ImageCoolStatus;
extern lv_obj_t *ui_TileMain;
extern lv_obj_t *ui_PinEntry;
extern lv_obj_t *ui_PinTextArea;
extern lv_obj_t *ui_Settings1;
extern lv_obj_t *ui_Main;
extern lv_obj_t *ui_DropMinTemp;
extern lv_obj_t *ui_DropMaxTemp;
extern lv_obj_t *ui_DropMode;
extern lv_obj_t *ui_DropCtrlType;
extern lv_obj_t *ui_DropHysteresis;
extern lv_obj_t *ui_DropStageStep;
extern lv_obj_t *ui_SpinSensorOffset;
extern lv_obj_t *ui_SliderBrightHigh;
extern lv_obj_t *ui_SliderBrightLow;
extern lv_obj_t *ui_DropTimeout;
extern lv_obj_t *ui_SpinModbusAddr;
extern lv_obj_t *ui_SwitchStartAp;
lv_obj_t *ui_DropSelectTheme = NULL;
static bool s_loading_settings = false;
#define EDIT_CFG (*settings_edit_config())
extern lv_obj_t *ui_ButtonDnd;
extern lv_obj_t *ui_ButtonMur;

extern bool s_last_displayed_dnd;
extern bool s_last_displayed_mur;

// Forward declarations for SquareLine-generated screens & helpers
extern void ui_PinEntry_screen_init(void);
extern void ui_Settings1_screen_init(void);
extern void ui_Settings2_screen_init(void);
extern void ui_Settings3_screen_init(void);
extern void ui_Main_screen_init(void);
extern void _ui_screen_change(lv_obj_t ** target, lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void));

// Settings3 init wrapper — initialise sliders from NVS values
static void ui_Settings3_screen_init_wrapped(void)
{
    s_loading_settings = true;
    ui_Settings3_screen_init();
    // Map NVS values (0-1023) → slider range (0-100)
    lv_slider_set_value(ui_SliderBrightHigh, (EDIT_CFG.bright_high * 100 + 511) / 1023, LV_ANIM_OFF);
    lv_slider_set_value(ui_SliderBrightLow,  (EDIT_CFG.bright_low * 100 + 511) / 1023, LV_ANIM_OFF);
    s_loading_settings = false;
}
// Forward declaration of helper function
static uint16_t get_dropdown_index_by_value(lv_obj_t *obj, int value, const int *table, size_t size);

// Called every time Settings1 screen becomes visible
void settings1_loaded_cb(lv_event_t *e)
{
    s_loading_settings = true;
    settings_edit_refresh();
    (void)e;
    LOG_C_INFO("[UI] Syncing Screen 1 widgets to RAM config...");
    if (ui_DropMinTemp) lv_dropdown_set_selected(ui_DropMinTemp, (uint16_t)(EDIT_CFG.temp_min - 10));
    if (ui_DropMaxTemp) lv_dropdown_set_selected(ui_DropMaxTemp, (uint16_t)(EDIT_CFG.temp_max - 25));
    if (ui_DropMode)    lv_dropdown_set_selected(ui_DropMode,    EDIT_CFG.hvac_mode);
    
    // Control Type: Index 0="1-Relay" (val 1), Index 1="3-Speed Fan" (val 0)
    if (ui_DropCtrlType) {
        lv_dropdown_set_selected(ui_DropCtrlType, (EDIT_CFG.ctrl_type == 0) ? 1 : 0);
    }
    s_loading_settings = false;
}

// Called every time Settings2 screen becomes visible — sync all advanced widgets
void settings2_loaded_cb(lv_event_t *e)
{
    s_loading_settings = true;
    settings_edit_refresh();
    (void)e;
    LOG_C_INFO("[UI] Syncing Screen 2 widgets to RAM config...");
    static const int hyst_table[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    if (ui_DropHysteresis) {
        lv_dropdown_set_selected(ui_DropHysteresis, get_dropdown_index_by_value(ui_DropHysteresis, EDIT_CFG.hysteresis_x10, hyst_table, 19));
    }

    static const int stage_table[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
    if (ui_DropStageStep) {
        lv_dropdown_set_selected(ui_DropStageStep, get_dropdown_index_by_value(ui_DropStageStep, EDIT_CFG.stage_step_x10, stage_table, 21));
    }

    if (ui_SpinSensorOffset) {
        lv_spinbox_set_value(ui_SpinSensorOffset, EDIT_CFG.sensor_offset_x10);
    }

    if (ui_DropSelectTheme) {
        lv_dropdown_set_selected(ui_DropSelectTheme, EDIT_CFG.theme_select);
    }
    s_loading_settings = false;
}

// Called every time Settings3 screen becomes visible (swipe or direct load)
void settings3_loaded_cb(lv_event_t *e)
{
    s_loading_settings = true;
    settings_edit_refresh();
    (void)e;
    LOG_C_INFO("[UI] Syncing Screen 3 widgets to RAM config...");
    if (ui_SliderBrightHigh) lv_slider_set_value(ui_SliderBrightHigh, (EDIT_CFG.bright_high * 100 + 511) / 1023, LV_ANIM_OFF);
    if (ui_SliderBrightLow)  lv_slider_set_value(ui_SliderBrightLow,  (EDIT_CFG.bright_low * 100 + 511) / 1023, LV_ANIM_OFF);

    static const uint8_t timeout_table[] = {30, 60, 120};
    if (ui_DropTimeout) {
        for(uint16_t i = 0; i < 3; i++) {
            if(timeout_table[i] == EDIT_CFG.timeout_s) {
                lv_dropdown_set_selected(ui_DropTimeout, i);
                break;
            }
        }
    }
    if (ui_SpinModbusAddr) lv_spinbox_set_value(ui_SpinModbusAddr, EDIT_CFG.modbus_addr);
    s_loading_settings = false;
}

// Forward declarations
void ui_sync_settings_to_widgets(void);

// ── Clean screen timer ────────────────────────────────────────────────────────
static lv_timer_t *s_clean_timer = NULL;
static uint8_t     s_clean_secs  = 60;

bool ui_clean_countdown_active(void)
{
    return (s_clean_timer != NULL);
}

static void clean_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_clean_secs > 0) {
        s_clean_secs--;
        lv_label_set_text_fmt(ui_LabelCleanCountdown, "%u", s_clean_secs);
    } else {
        lv_timer_del(s_clean_timer);
        s_clean_timer = NULL;
        lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_CLICKABLE);
        
        lv_obj_clear_flag(ui_LabelCleanMsg, LV_OBJ_FLAG_HIDDEN); // Prikazuje glavnu poruku
        lv_label_set_text(ui_LabelCleanCountdown, "");
        inactivity_force_timeout();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Navigation & Main Tile
// ═════════════════════════════════════════════════════════════════════════════

void action_go_to_thermo(lv_event_t *e)
{
    (void)e;
    // Stara logika skrolanja obrisana - SquareLine sada hendla promjenu ekrana
}

void action_dnd_toggled(lv_event_t * e)
{
    (void)e;
    inactivity_reset();
    bool checked = lv_obj_has_state(ui_ButtonDnd, LV_STATE_CHECKED);
    LOG_C_INFO("[UI] DND toggled %s", checked ? "ON" : "OFF");

    if (checked) {
        // DND is activated, so deactivate MUR
        lv_obj_clear_state(ui_ButtonMur, LV_STATE_CHECKED);
        modbus_set_mur_coil(false);
        s_last_displayed_mur = false;
        s_last_displayed_dnd = true;
        show_dnd_popup();
        LOG_C_INFO("[UI] MUR state cleared due to DND activation");
    } else {
        s_last_displayed_dnd = false;
    }
    modbus_set_dnd_coil(checked);
}

void action_mur_toggled(lv_event_t * e)
{
    (void)e;
    inactivity_reset();
    bool checked = lv_obj_has_state(ui_ButtonMur, LV_STATE_CHECKED);
    LOG_C_INFO("[UI] MUR toggled %s", checked ? "ON" : "OFF");

    if (checked) {
        // MUR is activated, so deactivate DND
        lv_obj_clear_state(ui_ButtonDnd, LV_STATE_CHECKED);
        modbus_set_dnd_coil(false);
        s_last_displayed_dnd = false;
        s_last_displayed_mur = true;
        show_mur_popup();
        LOG_C_INFO("[UI] DND state cleared due to MUR activation");
    } else {
        s_last_displayed_mur = false;
    }
    modbus_set_mur_coil(checked);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Thermostat Tile
// ═════════════════════════════════════════════════════════════════════════════

void action_arc_temp_changed(lv_event_t *e)
{
    (void)e;
    // Čitamo direktno vrijednost na koju je korisnik prevukao Arc
    int val = lv_arc_get_value(ui_ArcTemp);
    
    // Ažuriramo labelu na sredini Arca
    lv_label_set_text_fmt(ui_LabelTargetTemp, "%d°", val);
    hvac_set_setpoint(val);
    inactivity_reset(); // Spriječi gašenje ekrana dok korisnik podešava
}

void thermostat_loaded_cb(lv_event_t *e)
{
    (void)e;
    if (ui_ArcTemp) {
        lv_arc_set_range(ui_ArcTemp, g_sys_cfg.temp_min, g_sys_cfg.temp_max);
    }
}

void action_fan_speed_change(lv_event_t *e)
{
    (void)e;
    
    // Pročitaj trenutnu vrednost iz Modbus registra
    uint8_t current_speed = (uint8_t)g_mb.hreg[MB_REG_FAN_SPEED];
    
    LOG_C_INFO("[UI] Fan button clicked, current speed from Modbus: %u", current_speed);
    
    // Inkrementiraj cirkularno (0->1->2->3->0)
    current_speed = (current_speed + 1) % 4;
    
    LOG_C_INFO("[UI] New fan speed: %u", current_speed);
    
    // Ažuriraj tekst na labeli prema novoj vrednosti
    switch (current_speed) {
        case FAN_AUTO: lv_label_set_text(ui_LabelFanStatus, "Auto"); break;
        case FAN_LOW:  lv_label_set_text(ui_LabelFanStatus, "Low");  break;
        case FAN_MID:  lv_label_set_text(ui_LabelFanStatus, "Mid");  break;
        case FAN_HIGH: lv_label_set_text(ui_LabelFanStatus, "High"); break;
    }
    
    // Postavi novu vrednost u HVAC i Modbus
    hvac_set_fan_speed(current_speed);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clean Screen (Tile 4)
//  Press → timer starts immediately.
//  Hold 10 s → cancel timer, open PIN entry.
//  Release before 10 s → timer keeps running (normal clean).
// ═════════════════════════════════════════════════════════════════════════════

static unsigned long s_clean_press_ms = 0;

void action_clean_pressed(lv_event_t *e)
{
    (void)e;
    if (s_clean_timer) return;   // already running (second press)

    s_clean_press_ms = millis();
    s_clean_secs = 60;
    lv_obj_add_flag(lv_layer_sys(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_LabelCleanMsg, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(ui_LabelCleanCountdown, "%u", s_clean_secs);
    s_clean_timer = lv_timer_create(clean_timer_cb, 1000, NULL);
}

void action_clean_start(lv_event_t *e)
{
    (void)e;
    // Released — check if held long enough for hidden PIN entry
    unsigned long held = millis() - s_clean_press_ms;
    if (held >= 10000UL) {
        // 10 s hold → cancel timer, open PIN
        if (s_clean_timer) {
            lv_timer_del(s_clean_timer);
            s_clean_timer = NULL;
        }
        lv_obj_clear_flag(lv_layer_sys(), LV_OBJ_FLAG_CLICKABLE);
        
        // Reset clean screen visual state
        if (ui_LabelCleanMsg) lv_obj_clear_flag(ui_LabelCleanMsg, LV_OBJ_FLAG_HIDDEN);
        if (ui_LabelCleanCountdown) lv_label_set_text(ui_LabelCleanCountdown, "");

        inactivity_set_on_settings(true);
        _ui_screen_change(&ui_Settings1, LV_SCR_LOAD_ANIM_FADE_ON,
                          500, 0, &ui_Settings1_screen_init);
        ui_sync_settings_to_widgets();
        return;
    }
    // Otherwise timer keeps running (started in action_clean_pressed)
}

// ── Settings UI Synchronization ──────────────────────────────────────────────

// Helper to find index in dropdown by value
static uint16_t get_dropdown_index_by_value(lv_obj_t *obj, int value, const int *table, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (table[i] == value) return (uint16_t)i;
    }
    return 0;
}

void ui_sync_settings_to_widgets(void)
{
    const bool was_loading = s_loading_settings;
    s_loading_settings = true;
    settings_edit_refresh();
    LOG_C_INFO("[UI] Syncing widgets to RAM config...");

    // Screen 1
    if (ui_DropMinTemp) lv_dropdown_set_selected(ui_DropMinTemp, (uint16_t)(EDIT_CFG.temp_min - 10));
    if (ui_DropMaxTemp) lv_dropdown_set_selected(ui_DropMaxTemp, (uint16_t)(EDIT_CFG.temp_max - 25));
    if (ui_DropMode)    lv_dropdown_set_selected(ui_DropMode,    EDIT_CFG.hvac_mode);
    
    // Control Type: Index 0="1-Relay" (val 1), Index 1="3-Speed Fan" (val 0)
    if (ui_DropCtrlType) {
        lv_dropdown_set_selected(ui_DropCtrlType, (EDIT_CFG.ctrl_type == 0) ? 1 : 0);
    }

    // Screen 2
    static const int hyst_table[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    if (ui_DropHysteresis) {
        lv_dropdown_set_selected(ui_DropHysteresis, get_dropdown_index_by_value(ui_DropHysteresis, EDIT_CFG.hysteresis_x10, hyst_table, 19));
    }

    static const int stage_table[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
    if (ui_DropStageStep) {
        lv_dropdown_set_selected(ui_DropStageStep, get_dropdown_index_by_value(ui_DropStageStep, EDIT_CFG.stage_step_x10, stage_table, 21));
    }

    if (ui_SpinSensorOffset) lv_spinbox_set_value(ui_SpinSensorOffset, EDIT_CFG.sensor_offset_x10);

    // Screen 3
    if (ui_SliderBrightHigh) lv_slider_set_value(ui_SliderBrightHigh, (EDIT_CFG.bright_high * 100 + 511) / 1023, LV_ANIM_OFF);
    if (ui_SliderBrightLow)  lv_slider_set_value(ui_SliderBrightLow,  (EDIT_CFG.bright_low * 100 + 511) / 1023, LV_ANIM_OFF);
    
    static const uint8_t timeout_table[] = {30, 60, 120};
    if (ui_DropTimeout) {
        for(uint16_t i = 0; i < 3; i++) {
            if(timeout_table[i] == EDIT_CFG.timeout_s) {
                lv_dropdown_set_selected(ui_DropTimeout, i);
                break;
            }
        }
    }
    if (ui_SpinModbusAddr) lv_spinbox_set_value(ui_SpinModbusAddr, EDIT_CFG.modbus_addr);
    if (ui_DropSelectTheme) lv_dropdown_set_selected(ui_DropSelectTheme, EDIT_CFG.theme_select);
    s_loading_settings = was_loading;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PIN Entry
// ═════════════════════════════════════════════════════════════════════════════

void action_validate_pin(lv_event_t *e)
{
    (void)e;
    const char *pin = lv_textarea_get_text(ui_PinTextArea);
    if (strcmp(pin, "43962") == 0) {
        inactivity_set_on_settings(true);
        _ui_screen_change(&ui_Settings1, LV_SCR_LOAD_ANIM_FADE_ON,
                          500, 0, &ui_Settings1_screen_init);
    } else {
        _ui_screen_change(&ui_Main, LV_SCR_LOAD_ANIM_FADE_ON,
                          500, 0, &ui_Main_screen_init);
    }
    lv_textarea_set_text(ui_PinTextArea, "");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Settings 1 — Basic
// ═════════════════════════════════════════════════════════════════════════════

void action_min_temp_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    uint16_t sel = lv_dropdown_get_selected(ui_DropMinTemp);
    EDIT_CFG.temp_min = (int16_t)(10 + sel);
    settings_edit_mark(FLAG_TEMP_MIN);
}

void action_max_temp_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    uint16_t sel = lv_dropdown_get_selected(ui_DropMaxTemp);
    EDIT_CFG.temp_max = (int16_t)(25 + sel); // Corrected base
    settings_edit_mark(FLAG_TEMP_MAX);
}

void action_hvac_mode_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    uint16_t sel = lv_dropdown_get_selected(ui_DropMode);
    EDIT_CFG.hvac_mode = (uint8_t)sel;
    settings_edit_mark(FLAG_HVAC_MODE);

}

void action_ctrl_type_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    uint16_t sel = lv_dropdown_get_selected(ui_DropCtrlType);
    // Index 0="1-Relay" → Modbus 1, Index 1="3-Speed Fan" → Modbus 0
    uint8_t ctrl_val = (sel == 0) ? 1 : 0;
    EDIT_CFG.ctrl_type = ctrl_val;
    settings_edit_mark(FLAG_CTRL_TYPE);

}

// ═════════════════════════════════════════════════════════════════════════════
//  Settings 2 — Advanced
// ═════════════════════════════════════════════════════════════════════════════

void action_hysteresis_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    static const int hyst_table[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    uint16_t sel = lv_dropdown_get_selected(ui_DropHysteresis);
    if (sel < 19) {
        EDIT_CFG.hysteresis_x10 = (int16_t)hyst_table[sel];
        settings_edit_mark(FLAG_HYSTERESIS);
    }
}

void action_stage_step_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    static const int stage_table[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25};
    uint16_t sel = lv_dropdown_get_selected(ui_DropStageStep);
    if (sel < 21) {
        EDIT_CFG.stage_step_x10 = (int16_t)stage_table[sel];
        settings_edit_mark(FLAG_STAGE_STEP);
    }
}

void action_offset_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    int32_t val = lv_spinbox_get_value(ui_SpinSensorOffset);
    EDIT_CFG.sensor_offset_x10 = settings_clamp_sensor_offset(val);
    settings_edit_mark(FLAG_SENSOR_OFFSET);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Settings 2 — Theme Selection
// ═════════════════════════════════════════════════════════════════════════════

extern void apply_theme(uint8_t theme);

void action_theme_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    if (ui_DropSelectTheme) {
        uint16_t sel = lv_dropdown_get_selected(ui_DropSelectTheme);
        if (sel > 1) sel = 0;
        EDIT_CFG.theme_select = (uint8_t)sel;
        settings_edit_mark(FLAG_THEME_SELECT);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Settings 3 — System
// ═════════════════════════════════════════════════════════════════════════════

void action_bright_high_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    int32_t val = lv_slider_get_value(ui_SliderBrightHigh);
    uint16_t mapped = (uint16_t)((val * 1023 + 50) / 100);
    EDIT_CFG.bright_high = mapped;
    settings_edit_mark(FLAG_BRIGHT_HIGH);
    hal_backlight_set(mapped);
}

void action_bright_low_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    int32_t val = lv_slider_get_value(ui_SliderBrightLow);
    EDIT_CFG.bright_low = (uint16_t)((val * 1023 + 50) / 100);
    settings_edit_mark(FLAG_BRIGHT_LOW);
}

void action_timeout_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    static const uint8_t timeout_table[] = {30, 60, 120};
    uint16_t sel = lv_dropdown_get_selected(ui_DropTimeout);
    if (sel < sizeof(timeout_table))
        EDIT_CFG.timeout_s = timeout_table[sel];
    settings_edit_mark(FLAG_TIMEOUT);
}

void action_modbus_changed(lv_event_t *e)
{
    if (s_loading_settings) return;
    (void)e;
    inactivity_reset();
    int32_t val = lv_spinbox_get_value(ui_SpinModbusAddr);
    if (val < 1)   val = 1;
    if (val > 247) val = 247;
    EDIT_CFG.modbus_addr = (uint8_t)val;
    settings_edit_mark(FLAG_MODBUS_ADDR);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Save & Exit
// ═════════════════════════════════════════════════════════════════════════════

void action_save_and_exit(lv_event_t *e)
{
    (void)e;
    if (!settings_edit_save()) {
        lv_obj_t *msg=lv_msgbox_create(NULL, "SAVE", "Saving failed. Retry SAVE.", NULL, true);
        lv_obj_center(msg);
        return;
    }
    modbus_set_slave_addr(g_sys_cfg.modbus_addr);  // apply new address immediately
    inactivity_set_on_settings(false);
    _ui_screen_change(&ui_Main, LV_SCR_LOAD_ANIM_FADE_ON,
                      500, 0, &ui_Main_screen_init);
    apply_theme(g_sys_cfg.theme_select);
}

// ═════════════════════════════════════════════════════════════════════════════
//  WiFi AP Manager
// ═════════════════════════════════════════════════════════════════════════════

void action_start_wifi_manager(lv_event_t *e)
{
    (void)e;
    bool ap_on = lv_obj_has_state(ui_SwitchStartAp, LV_STATE_CHECKED);
    g_wifi_ap_active = ap_on;
    inactivity_reset();
    wifi_manager_set_ap(ap_on);
}

// ═════════════════════════════════════════════════════════════════════════════
//  DND & MUR Popup Notifications (German)
// ═════════════════════════════════════════════════════════════════════════════

static lv_obj_t * s_current_popup = NULL;
static lv_timer_t * s_popup_timer = NULL;

static void popup_timer_cb(lv_timer_t * t)
{
    if (s_current_popup) {
        lv_obj_del(s_current_popup);
        s_current_popup = NULL;
    }
    s_popup_timer = NULL;
    lv_timer_del(t);
}

void clear_active_popup(void)
{
    if (s_popup_timer) {
        lv_timer_del(s_popup_timer);
        s_popup_timer = NULL;
    }
    if (s_current_popup) {
        lv_obj_del(s_current_popup);
        s_current_popup = NULL;
    }
}

void show_dnd_popup(void)
{
    if (lv_scr_act() != ui_Main) return;

    clear_active_popup();

    // Create container on Main Screen
    s_current_popup = lv_obj_create(ui_Main);
    lv_obj_set_size(s_current_popup, 360, 110);
    lv_obj_align(s_current_popup, LV_ALIGN_CENTER, 0, -45);

    // Glassmorphism styling (same style as other popups but in vibrant neon green)
    lv_obj_set_style_bg_color(s_current_popup, lv_color_hex(0x141419), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_current_popup, 230, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_current_popup, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Green border (width 3, vibrant emerald/neon green)
    lv_obj_set_style_border_color(s_current_popup, lv_color_hex(0x00E676), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_current_popup, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_current_popup, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Green shadow glow
    lv_obj_set_style_shadow_color(s_current_popup, lv_color_hex(0x00E676), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_current_popup, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(s_current_popup, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(s_current_popup, 1, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_clear_flag(s_current_popup, LV_OBJ_FLAG_SCROLLABLE);

    // Elegant text inside
    lv_obj_t * label = lv_label_create(s_current_popup);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, "BITTE NICHT STOEREN\nAKTIVIERT");
    lv_obj_set_width(label, 320);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // 3-second self-destruct timer
    s_popup_timer = lv_timer_create(popup_timer_cb, 3000, s_current_popup);
}

void show_mur_popup(void)
{
    if (lv_scr_act() != ui_Main) return;

    clear_active_popup();

    // Create container on Main Screen
    s_current_popup = lv_obj_create(ui_Main);
    lv_obj_set_size(s_current_popup, 360, 110);
    lv_obj_align(s_current_popup, LV_ALIGN_CENTER, 0, -45);

    // Glassmorphism styling (same style as other popups but in vibrant neon green)
    lv_obj_set_style_bg_color(s_current_popup, lv_color_hex(0x141419), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_current_popup, 230, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_current_popup, 16, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Green border (width 3, vibrant emerald/neon green)
    lv_obj_set_style_border_color(s_current_popup, lv_color_hex(0x00E676), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_current_popup, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_current_popup, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Green shadow glow
    lv_obj_set_style_shadow_color(s_current_popup, lv_color_hex(0x00E676), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_current_popup, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(s_current_popup, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_spread(s_current_popup, 1, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_clear_flag(s_current_popup, LV_OBJ_FLAG_SCROLLABLE);

    // Elegant text inside
    lv_obj_t * label = lv_label_create(s_current_popup);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, "ZIMMERREINIGUNG\nANGEFORDERT");
    lv_obj_set_width(label, 320);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // 3-second self-destruct timer
    s_popup_timer = lv_timer_create(popup_timer_cb, 3000, s_current_popup);
}

// Refresh untouched fields after remote writes; retain locally edited fields.
void ui_settings_poll(void)
{
    if (settings_edit_refresh()) ui_sync_settings_to_widgets();
    static uint16_t last_level = 0xffff;
    uint16_t level = inactivity_is_screensaver_active() ? g_sys_cfg.bright_low :
        (inactivity_on_settings_screen() ? EDIT_CFG.bright_high : g_sys_cfg.bright_high);
    if (level != last_level) { hal_backlight_set(level); last_level=level; }
    apply_theme(g_sys_cfg.theme_select);
}
