#include <Arduino.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <lvgl.h>

#include "hal.h"
#include "settings.h"
#include "modbus_handler.h"
#include "hvac.h"
#include "debug_logger.h"
#include "wifi_manager.h"
#include "version.h"

extern "C" {
    #include "../lvgl/ui.h"
    #include "../lvgl/ui_events.h"
    // Add event handlers for DND/MUR
    extern lv_obj_t * ui_ButtonDnd;
    extern lv_obj_t * ui_ButtonMur;
    void action_dnd_toggled(lv_event_t * e);
    void action_mur_toggled(lv_event_t * e);
    void settings3_loaded_cb(lv_event_t *e);
    void ui_settings_poll(void);
    void action_arc_temp_changed(lv_event_t * e);
}

// Coil mirrors from modbus_handler
extern bool g_mb_coil_dnd;
extern bool g_mb_coil_mur;
// Rename local shadows to use extern names
#define s_mb_coil_dnd g_mb_coil_dnd
#define s_mb_coil_mur g_mb_coil_mur

// ── Timing constants ──────────────────────────────────────────────────────────
#define HVAC_UPDATE_MS         500UL   // NTC read + relay logic period
#define CLOCK_UPDATE_MS       1000UL   // clock label refresh period
#define INACTIVITY_CHECK_MS   1000UL   // inactivity poll period

// Timezone constant for Sarajevo (CET/CEST with DST transitions)
#define SARAJEVO_TZ "CET-1CEST,M3.5.0,M10.5.0/3"

// ── Timestamps ────────────────────────────────────────────────────────────────
static unsigned long t_hvac      = 0;
static unsigned long t_clock     = 0;
static unsigned long t_inact     = 0;
static unsigned long s_boot_ms   = 0;

// ── Window open popup ─────────────────────────────────────────────────────────
static lv_obj_t *s_window_popup = NULL;
static bool      s_window_was_open = false;

// ── Custom logo (LittleFS PNG) ────────────────────────────────────────────────
static lv_obj_t *s_logo_img = NULL;

extern "C" void apply_theme(uint8_t theme)
{
    switch (theme) {
        case 0: // NONE
            if (s_logo_img) lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
            break;
        case 1: // LOGO
            if (s_logo_img) lv_obj_clear_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

// ── Time Sync State Machine ───────────────────────────────────────────────────
static bool s_is_clock_set = false;
static unsigned long s_last_sync_ms = 0;
static bool s_initial_sync_pending = true;

extern "C" void on_time_synced(const char* source) {
    s_is_clock_set = true;
    s_last_sync_ms = millis();
    s_initial_sync_pending = false; // No need for initial sync anymore
    LOG_INFO("[TIME] Clock synchronized via %s", source);
}

void try_ntp_sync() {
    if (g_wifi_ap_active) {
        LOG_INFO("[NTP] Sync skipped, WiFi AP is active.");
        return;
    }

    if (!wifi_manager_has_credentials()) {
        LOG_INFO("[NTP] Sync skipped, no WiFi credentials saved.");
        return;
    }

    LOG_INFO("[NTP] Starting NTP sync attempt...");
    
    bool was_screensaver = inactivity_is_screensaver_active();
    if (getCpuFrequencyMhz() < 160) {
        setCpuFrequencyMhz(240);
        LOG_INFO("[NTP] CPU clock restored to 240MHz for WiFi.");
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin();

    unsigned long connect_start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if (millis() - connect_start > 20000) { // 20s connect timeout
            LOG_ERROR("[NTP] WiFi connection timed out.");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            if (was_screensaver) {
                setCpuFrequencyMhz(80);
                LOG_INFO("[NTP] CPU clock returned to 80MHz.");
            }
            return;
        }
    }
    LOG_INFO("[NTP] WiFi connected: %s", WiFi.localIP().toString().c_str());

    configTzTime(SARAJEVO_TZ, "pool.ntp.org", "time.nist.gov");
    
    unsigned long ntp_start = millis();
    while (time(nullptr) < 1672531200) { // Check if time is later than 2023-01-01
        delay(100);
        if (millis() - ntp_start > 10000) { // 10s NTP timeout
            LOG_ERROR("[NTP] NTP time sync timed out.");
            break;
        }
    }

    if (time(nullptr) > 1672531200) {
        on_time_synced("NTP");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    LOG_INFO("[NTP] WiFi disconnected, sync process finished.");

    if (was_screensaver) {
        setCpuFrequencyMhz(80);
        LOG_INFO("[NTP] CPU clock returned to 80MHz.");
    }
}

// ── Clock helpers ─────────────────────────────────────────────────────────────
static void update_clock_label(void)
{
    if (!s_is_clock_set) {
        unsigned long s = millis() / 1000;
        unsigned int  h = (s / 3600) % 24;
        unsigned int  m = (s / 60)   % 60;
        lv_label_set_text_fmt(ui_LabelClock, "%02u:%02u", h, m);
    } else {
        time_t now_time = time(NULL);
        struct tm *t = localtime(&now_time);
        lv_label_set_text_fmt(ui_LabelClock, "%02d:%02d", t->tm_hour, t->tm_min);
    }
}

// ── Room temperature → UI ────────────────────────────────────────────────────
static void update_temp_labels(void)
{
    if (hvac_temp_sensor_fault()) {
        lv_label_set_text(ui_LabelCurrentVal, "--\xC2\xB0" "C");
        lv_label_set_text(ui_LabelRoomTemp, "Innen:  --\xC2\xB0" "C");
        return;
    }

    float t = hvac_get_room_temp()
              + (g_sys_cfg.sensor_offset_x10 / 10.0f);
    char temp_buf[32]; // Buffer for formatted strings

    // Main screen value label (text label remains static)
    snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", roundf(t));
    lv_label_set_text(ui_LabelCurrentVal, temp_buf);

    // Thermostat screen label (one line, no spaces)
    snprintf(temp_buf, sizeof(temp_buf), "Innen:  %.0f°C", roundf(t));
    lv_label_set_text(ui_LabelRoomTemp, temp_buf);

}

// ── HVAC mode images ─────────────────────────────────────────────────────────
// Read the applied relay outputs, including AUTO and the all-off deadband.
static void update_active_fan_indicator(void)
{
    if (!ui_LabelActiveFanSpeed) return;
    const char *text = "";
    if (g_mb.hreg[MB_REG_RELAY_MODE] == 0) {
        unsigned mask = (hal_relay_is_on(1) ? 1U : 0U) |
                        (hal_relay_is_on(2) ? 2U : 0U) |
                        (hal_relay_is_on(3) ? 4U : 0U);
        if (mask == 1) text = "I";
        else if (mask == 2) text = "II";
        else if (mask == 4) text = "III";
    }
    if (strcmp(lv_label_get_text(ui_LabelActiveFanSpeed), text) != 0)
        lv_label_set_text(ui_LabelActiveFanSpeed, text);
}

static void update_hvac_icons(void)
{
    uint8_t mode = g_mb.hreg[MB_REG_HVAC_MODE];
    lv_obj_add_flag(ui_ImageHeatStatus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_ImageCoolStatus, LV_OBJ_FLAG_HIDDEN);
    if (mode == HVAC_HEAT)
        lv_obj_clear_flag(ui_ImageHeatStatus, LV_OBJ_FLAG_HIDDEN);
    else if (mode == HVAC_COOL)
        lv_obj_clear_flag(ui_ImageCoolStatus, LV_OBJ_FLAG_HIDDEN);
}

// ── Thermostat widget sync (Modbus → GUI) ────────────────────────────────────
// Detects when Modbus registers change (master write or boot) and updates
// Arc, fan label, and DND/MUR buttons. Called every HVAC_UPDATE_MS period.
static uint16_t s_last_displayed_setpoint = 0xFFFF;  // sentinel: force first update
static uint8_t  s_last_displayed_fan      = 0xFF;
bool     s_last_displayed_dnd      = false;
bool     s_last_displayed_mur      = false;

static void update_thermostat_widgets(void)
{
    // ── Setpoint Arc & Label ──────────────────────────────────────────────────
    // Skip Modbus override while the user is actively dragging the arc.
    // LV_STATE_PRESSED is set by LVGL for the entire duration of the touch.
    uint16_t mb_setpoint_x10 = g_mb.hreg[MB_REG_TARGET_TEMP];
    if (!lv_obj_has_state(ui_ArcTemp, LV_STATE_PRESSED) &&
        (mb_setpoint_x10 != s_last_displayed_setpoint ||
         lv_arc_get_min_value(ui_ArcTemp) != g_sys_cfg.temp_min ||
         lv_arc_get_max_value(ui_ArcTemp) != g_sys_cfg.temp_max ||
         lv_arc_get_value(ui_ArcTemp) != mb_setpoint_x10 / 10)) {
        s_last_displayed_setpoint = mb_setpoint_x10;
        int sp = (int)(mb_setpoint_x10 / 10);
        // Remove only our specific callback to avoid silently discarding any
        // future callbacks registered on this object.
        lv_obj_remove_event_cb(ui_ArcTemp, action_arc_temp_changed);
        lv_arc_set_range(ui_ArcTemp, g_sys_cfg.temp_min, g_sys_cfg.temp_max);
        lv_arc_set_value(ui_ArcTemp, sp);
        lv_obj_add_event_cb(ui_ArcTemp, action_arc_temp_changed, LV_EVENT_VALUE_CHANGED, NULL);
        lv_label_set_text_fmt(ui_LabelTargetTemp, "%d°", sp);
    }

    // ── Fan Speed Label ───────────────────────────────────────────────────────
    uint8_t mb_fan = (uint8_t)g_mb.hreg[MB_REG_FAN_SPEED];
    if (mb_fan != s_last_displayed_fan) {
        s_last_displayed_fan = mb_fan;
        switch (mb_fan) {
            case FAN_AUTO: lv_label_set_text(ui_LabelFanStatus, "Auto"); break;
            case FAN_LOW:  lv_label_set_text(ui_LabelFanStatus, "Low");  break;
            case FAN_MID:  lv_label_set_text(ui_LabelFanStatus, "Mid");  break;
            case FAN_HIGH: lv_label_set_text(ui_LabelFanStatus, "High"); break;
            default:       lv_label_set_text(ui_LabelFanStatus, "Auto"); break;
        }
    }

    // ── DND / MUR Coils ───────────────────────────────────────────────────────
    bool mb_dnd = s_mb_coil_dnd;
    bool mb_mur = s_mb_coil_mur;
    if (mb_dnd != s_last_displayed_dnd) {
        s_last_displayed_dnd = mb_dnd;
        if (mb_dnd) {
            lv_obj_add_state(ui_ButtonDnd, LV_STATE_CHECKED);
            show_dnd_popup();
        } else {
            lv_obj_clear_state(ui_ButtonDnd, LV_STATE_CHECKED);
        }
    }
    if (mb_mur != s_last_displayed_mur) {
        s_last_displayed_mur = mb_mur;
        if (mb_mur) {
            lv_obj_add_state(ui_ButtonMur, LV_STATE_CHECKED);
            show_mur_popup();
        } else {
            lv_obj_clear_state(ui_ButtonMur, LV_STATE_CHECKED);
        }
    }
}

// ── Outside temperature → UI ───────────────────────────────────────────────────
// Placeholder for future weather integration
static void update_outside_temp_label(void)
{
    if (modbus_has_outside_temp()) {
        lv_obj_clear_flag(ui_LabelOutdoorTemp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_LabelOutdoorVal, LV_OBJ_FLAG_HIDDEN);
        // Cast uint16_t -> int16_t -> int da bi ispravno očitali negativne temperature (npr. -5°C)
        int out_temp = (int)(int16_t)g_mb.hreg[MB_REG_OUTSIDE_TEMP];
        char temp_buf[32];
        snprintf(temp_buf, sizeof(temp_buf), "%d°C", out_temp);
        lv_label_set_text(ui_LabelOutdoorVal, temp_buf);
    } else {
        lv_obj_add_flag(ui_LabelOutdoorTemp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_LabelOutdoorVal, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Window open popup ──────────────────────────────────────────────────────────
static void window_popup_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_window_popup) {
        lv_obj_del(s_window_popup);
        s_window_popup = NULL;
    }
}

static void window_popup_create(void)
{
    if (s_window_popup) return;

    s_window_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_window_popup, 440, 320);
    lv_obj_center(s_window_popup);
    lv_obj_set_style_border_width(s_window_popup, 3, 0);
    lv_obj_set_style_border_color(s_window_popup, lv_color_hex(0xFF0000), 0);

    lv_obj_t *label_title = lv_label_create(s_window_popup);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(label_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_title, "ACHTUNG !!!");
    lv_obj_set_width(label_title, 400);
    lv_obj_align(label_title, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *label_body = lv_label_create(s_window_popup);
    lv_obj_set_style_text_font(label_body, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(label_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label_body, "DIE KLIMAANLAGE IST DEAKTIVIERT.\nBITTE SCHLIESSEN SIE TUEREN\nUND FENSTER.");
    lv_label_set_long_mode(label_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_body, 380);
    lv_obj_align(label_body, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *btn = lv_btn_create(s_window_popup);
    lv_obj_set_width(btn, 180);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_add_event_cb(btn, window_popup_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(btn_label, "SCHLIESSEN");
    lv_obj_center(btn_label);
}

static void window_popup_destroy(void)
{
    if (s_window_popup) {
        lv_obj_del(s_window_popup);
        s_window_popup = NULL;
    }
}

// ── Custom logo display ───────────────────────────────────────────────────────
// Reads the PNG file from LittleFS into PSRAM, decodes it manually via lodepng
// to RGBA8888, then converts to LVGL TRUE_COLOR_ALPHA (RGB565 + α, 3 B/px) so
// that the image has correct dimensions and working transparency.
//
// The descriptor and pixel buffer are allocated once and intentionally never
// freed — LVGL keeps a reference to them for the lifetime of the logo widget.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h, const unsigned char* in, size_t insize);

static void init_custom_logo(void)
{
    // Create image widget once
    if (s_logo_img == NULL) {
        s_logo_img = lv_img_create(ui_TileMain);
        lv_obj_set_pos(s_logo_img, 0, 0);
        lv_obj_set_size(s_logo_img, 480, 100);
        lv_obj_move_to_index(s_logo_img, 0);
        lv_obj_set_style_pad_all(s_logo_img, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_logo_img, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_logo_img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(s_logo_img, 0, LV_PART_MAIN);
    }

    // Try to open logo file
    File f = LittleFS.open("/logo_480x100.png", FILE_READ);
    if (!f) {
        LOG_INFO("[LOGO] Nema fajla, widget skriven.");
        lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    size_t fsize = f.size();
    LOG_INFO("[LOGO] Citam %u B...", fsize);
    uint8_t *filedata = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);

    bool ok = false;
    if (filedata) {
        f.read(filedata, fsize);
        f.close();

        unsigned w, h;
        uint8_t *rgba;
        unsigned err = lodepng_decode32(&rgba, &w, &h, filedata, fsize);
        free(filedata);

        if (err == 0) {
            LOG_INFO("[LOGO] lodepng ok: %u x %u", w, h);
            size_t px = (size_t)w * h;
            size_t out_sz = px * 3;
            uint8_t *out = (uint8_t *)heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM);

            if (out) {
                for (size_t i = 0; i < px; i++) {
                    uint16_t rgb = ((rgba[i*4]   >> 3) << 11)
                                 | ((rgba[i*4+1] >> 2) <<  5)
                                 |  (rgba[i*4+2] >> 3);
                    out[i*3]     =  rgb       & 0xFF;
                    out[i*3 + 1] = (rgb >> 8) & 0xFF;
                    out[i*3 + 2] =  rgba[i*4+3];
                }
                free(rgba);

                lv_img_dsc_t *dsc = (lv_img_dsc_t *)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM);
                if (dsc) {
                    memset(dsc, 0, sizeof(lv_img_dsc_t));
                    dsc->header.w  = (int16_t)w;
                    dsc->header.h  = (int16_t)h;
                    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                    dsc->data      = out;
                    dsc->data_size = out_sz;
                    lv_img_set_src(s_logo_img, dsc);
                    lv_obj_clear_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
                    LOG_INFO("[LOGO] TRUE_COLOR_ALPHA %ux%u (%u B)", w, h, out_sz);
                    ok = true;
                } else {
                    free(out);
                    LOG_ERROR("[LOGO] malloc dsc fail");
                }
            } else {
                free(rgba);
                LOG_ERROR("[LOGO] malloc out fail");
            }
        } else {
            LOG_ERROR("[LOGO] lodepng err %u", err);
        }
    } else {
        f.close();
        LOG_ERROR("[LOGO] malloc file buf fail");
    }

    if (!ok) {
        lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── setup ────────────────────────────────────────────────────────────────────
void setup(void)
{
    Serial.begin(115200);
    delay(300);
    
    s_boot_ms = millis();

    LOG_INFO("=== Hotel Room Thermostat v%d.%d.%d (%s) ===", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, BUILD_DATE);
    LOG_INFO("CPU: %lu MHz   PSRAM: %.1f MB free",
             getCpuFrequencyMhz(),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);

    // 0. Mount LittleFS (format on first boot if empty)
    if (!LittleFS.begin(true, "/littlefs")) {
        LOG_ERROR("[FS] LittleFS mount FAILED — logo upload nedostupan!");
    } else {
        LOG_INFO("[FS] LittleFS mounted, free: %u bajta",
                 LittleFS.totalBytes() - LittleFS.usedBytes());
    }

    // 1. Load NVS configuration
    settings_init();
    LOG_INFO("[MAIN] Settings loaded");

    // 1.5 Init WiFi Manager
    wifi_manager_init();

    // 2. Initialise all hardware + LVGL
    board_hal_init();
    LOG_INFO("[MAIN] HAL init done");

    // 3. Apply stored backlight level
    hal_backlight_set(g_sys_cfg.bright_high);
    LOG_INFO("[MAIN] Backlight set to %u", g_sys_cfg.bright_high);

    // 4. Start LVGL UI (SquareLine generated)
    LOG_INFO("[MAIN] Starting UI init...");
    ui_init();
    LOG_INFO("[MAIN] UI init done");
    LOG_INFO("[MAIN] Free heap: %.1f KB  PSRAM: %.1f MB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0f,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0f);

    // 4.1 Load custom logo from LittleFS (crta se na (0,0), crna pozadina)
    init_custom_logo();
    // 4.2 Apply selected theme (NONE / AURORA / LOGO)
    apply_theme(g_sys_cfg.theme_select);
    LOG_INFO("[MAIN] Custom logo init done");

    // Try one lv_timer_handler call here to catch crash early
    LOG_INFO("[MAIN] First lv_timer_handler call...");
    lv_timer_handler();
    LOG_INFO("[MAIN] First lv_timer_handler done");

    // 5. Modbus RTU slave
    modbus_init(g_sys_cfg.modbus_addr);

    // 6. HVAC / thermostat logic (uses g_sys_cfg and g_mb)
    hvac_init();

    // 7. Seed arc setpoint from NVS — use obj flag to suppress VALUE_CHANGED event
    int sp = g_mb.hreg[MB_REG_TARGET_TEMP] / 10;
    lv_obj_remove_event_cb(ui_ArcTemp, NULL);  // detach all callbacks temporarily
    lv_arc_set_value(ui_ArcTemp, sp);          // set value without triggering hvac_set_setpoint
    lv_obj_add_event_cb(ui_ArcTemp, action_arc_temp_changed, LV_EVENT_VALUE_CHANGED, NULL);  // re-attach
    lv_label_set_text_fmt(ui_LabelTargetTemp, "%d°", sp);
    // Seed the mirror so update_thermostat_widgets() skips on first loop iteration
    s_last_displayed_setpoint = (uint16_t)(sp * 10);
    
    // Set initial fan speed label from Modbus register
    uint8_t fan_speed = (uint8_t)g_mb.hreg[MB_REG_FAN_SPEED];
    switch (fan_speed) {
        case FAN_AUTO: lv_label_set_text(ui_LabelFanStatus, "Auto"); break;
        case FAN_LOW:  lv_label_set_text(ui_LabelFanStatus, "Low");  break;
        case FAN_MID:  lv_label_set_text(ui_LabelFanStatus, "Mid");  break;
        case FAN_HIGH: lv_label_set_text(ui_LabelFanStatus, "High"); break;
        default:       lv_label_set_text(ui_LabelFanStatus, "Auto"); break;
    }
    s_last_displayed_fan = fan_speed;  // seed mirror
    
    // 9. Initialize Settings3 controls on first screen creation.
    // Screen-loaded callback is already attached in generated LVGL code.
    settings3_loaded_cb(NULL);

    // 10. DND/MUR handlers are attached in generated LVGL screen code.
    LOG_INFO("[MAIN] DND/MUR event handlers ready.");

    LOG_INFO("[MAIN] Setup complete");
}

// ── loop ─────────────────────────────────────────────────────────────────────
void loop(void)
{
    unsigned long now = millis();

    // Deadband relay timer — runs every loop() for accurate RELAY_DEADBAND_MS timing
    hvac_deadband_tick();
    update_active_fan_indicator();

    // LVGL task handler (must be called frequently)
    lv_timer_handler();

    // Modbus poll
    modbus_poll();

    // HVAC update (NTC + relay)
    if (now - t_hvac >= HVAC_UPDATE_MS) {
        t_hvac = now;
        hvac_update();
        update_active_fan_indicator();
        update_temp_labels();
        update_hvac_icons();
        update_thermostat_widgets();  // sync Modbus→GUI for setpoint, fan, DND/MUR
        // Update Modbus input registers with current system state
        modbus_update_inputs();
        
        modbus_check_outside_temp_timeout();
        update_outside_temp_label();
    }

    // Time Sync Logic
    if (s_initial_sync_pending && (now - s_boot_ms > 15000UL)) {
        LOG_INFO("[TIME] 15s boot delay passed, attempting initial NTP sync.");
        s_initial_sync_pending = false; // Attempt only once
        try_ntp_sync();
    }

    if (s_is_clock_set && (now - s_last_sync_ms > 3600000UL)) { // 60 minutes
        LOG_INFO("[TIME] 60min since last sync, attempting fallback NTP sync.");
        try_ntp_sync();
    }

    // Clock refresh
    if (now - t_clock >= CLOCK_UPDATE_MS) {
        t_clock = now;
        update_clock_label();
    }

    // Inactivity check (settings screens)
    if (now - t_inact >= INACTIVITY_CHECK_MS) {
        t_inact = now;
        inactivity_check();
    }

    // Obrada WebServer zahtjeva za OTA (samo kad je AP uključen)
    if (g_wifi_ap_active) {
        wifi_manager_tick();
    }

    // Deferred NVS save — fires once, 3 s after the last settings change
    settings_tick();
    ui_settings_poll();

    // Window open popup management
    bool window_open = hvac_is_window_open();
    if (window_open != s_window_was_open) {
        s_window_was_open = window_open;
        if (window_open)
            window_popup_create();
        else
            window_popup_destroy();
    }

    // Show/hide popup based on active screen (hidden on settings)
    if (s_window_popup) {
        lv_obj_t *active = lv_scr_act();
        bool on_settings = (active == ui_Settings1) ||
                           (active == ui_Settings2) ||
                           (active == ui_Settings3);
        if (on_settings)
            lv_obj_add_flag(s_window_popup, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(s_window_popup, LV_OBJ_FLAG_HIDDEN);
    }

    // Small yield to prevent WDT reset
    delay(2);
}
