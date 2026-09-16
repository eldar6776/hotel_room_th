#include "ui_mode_hold.h"
#include "ui.h"
#include "hvac.h"
#include "modbus_handler.h"
#include "settings.h"

static lv_obj_t *s_zone;
static uint32_t s_pressed_at;
static bool s_tracking;

static void cancel_hold(void)
{
    s_tracking = false;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_wait_release(indev);
}

static void position_zone(void)
{
    if (!s_zone || !ui_ImageHeatStatus) return;
    lv_obj_update_layout(ui_Thermostat);
    lv_area_t screen, content, icon;
    lv_obj_get_coords(ui_Thermostat, &screen);
    lv_obj_get_content_coords(ui_Thermostat, &content);
    lv_obj_get_coords(ui_ImageHeatStatus, &icon);
    // Cover the icon and extend to the physical top and right screen edges.
    lv_obj_set_pos(s_zone, icon.x1 - content.x1, screen.y1 - content.y1);
    lv_obj_set_size(s_zone, screen.x2 - icon.x1 + 1, icon.y2 - screen.y1 + 1);
}

static void screen_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) position_zone();
    else if (code == LV_EVENT_SCREEN_UNLOAD_START) cancel_hold();
}

static void zone_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) { s_tracking = false; s_zone = NULL; return; }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        cancel_hold();
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER ||
        lv_scr_act() != ui_Thermostat) { cancel_hold(); return; }
    lv_point_t point;
    lv_area_t bounds;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(s_zone, &bounds);
    if (point.x < bounds.x1 || point.x > bounds.x2 ||
        point.y < bounds.y1 || point.y > bounds.y2) {
        cancel_hold();
        return;
    }
    inactivity_reset();
    if (code == LV_EVENT_PRESSED) {
        s_pressed_at = lv_tick_get();
        s_tracking = true;
    } else if (s_tracking && (uint32_t)(lv_tick_get() - s_pressed_at) > 5000U) {
        // Consume this touch until physical release, even if the finger moves.
        cancel_hold();
        uint8_t current = (uint8_t)g_mb.hreg[MB_REG_HVAC_MODE];
        uint8_t next = current == HVAC_OFF ? HVAC_HEAT :
                       current == HVAC_HEAT ? HVAC_COOL : HVAC_OFF;
        // Existing path updates HVAC safely, Modbus, dirty flag and deferred NVS save.
        hvac_set_mode(next);
    }
}

void ui_mode_hold_init(void)
{
    s_tracking = false;
    s_zone = lv_obj_create(ui_Thermostat);
    lv_obj_remove_style_all(s_zone);
    lv_obj_clear_flag(s_zone, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                      LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(s_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_zone, zone_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Thermostat, screen_event, LV_EVENT_ALL, NULL);
    position_zone();
}
