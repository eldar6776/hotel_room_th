#include "wifi_manager.h"
#include "debug_logger.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <Update.h>
#include <LittleFS.h>
#include <lvgl.h>

extern lv_obj_t *ui_SwitchStartAp;

static WiFiManager wm;
static bool s_portal_running = false;

static void register_logo_upload_route();

// Link na portalu — samo <a>, bez <form>, bez gniježđenja
static WiFiManagerParameter upload_link(
    "<hr>"
    "<a href='/upload' target='_blank'"
    "  style='display:block;padding:12px;background:#0078d7;color:#fff;"
    "         text-align:center;border-radius:5px;text-decoration:none;"
    "         font-size:16px;margin:8px 0;'>"
    "&#128247; Upload Logo Baner</a>"
);

void wifi_manager_init() {
    wm.setDebugOutput(false); // Disable WiFiManager internal logging to Serial
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(180); // 3 minutes
    
    // Configure WiFiManager menu to include the built-in firmware Update option
    std::vector<const char *> menu = {"wifi", "info", "param", "close", "sep", "update"};
    wm.setMenu(menu);

    wm.addParameter(&upload_link);
    wm.setWebServerCallback(register_logo_upload_route);
}

// ── Logo Upload Handler ───────────────────────────────────────────────────────
// Registrira /upload (GET) i /upload_logo (POST) rute na WiFiManager web serveru.
// Upload forma je na posebnoj stranici (/upload) — ne ugnježduje se unutar
// WiFiManagerove forme (HTML zabranjuje <form> unutar <form>).
static void register_logo_upload_route() {
    if (!wm.server.get()) return;

    // ── GET /upload — standalone HTML upload forma ──────────────────────────
    wm.server.get()->on("/upload", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
        html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<title>Upload Logo</title>";
        html += "<style>"
                "body{font-family:Arial;padding:20px;max-width:500px;margin:auto}"
                "input,button{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}"
                "button{background:#0078d7;color:#fff;border:none;border-radius:5px;font-size:16px;cursor:pointer}"
                ".msg{padding:12px;border-radius:5px;margin:10px 0}"
                ".ok{background:#d4edda;color:#155724}"
                ".err{background:#f8d7da;color:#721c24}"
                "</style></head><body>";
        html += "<h3>&#128247; Logo baner &mdash; 480×100 px PNG</h3>";
        html += "<form method='POST' action='/upload_logo' enctype='multipart/form-data'>";
        html += "<input type='file' name='logo_file' accept='.png' required>";
        html += "<button type='submit'>Ucitaj PNG u Flash</button>";
        html += "</form>";
        html += "<p style='color:#666;font-size:14px'>Uredjaj ce se restartovati nakon uploada.</p>";
        html += "</body></html>";
        wm.server.get()->send(200, "text/html; charset=utf-8", html);
    });

    // ── POST /upload_logo — multipart upload handler ───────────────────────
    wm.server.get()->on("/upload_logo", HTTP_POST,
        /* completion callback — poziva se kad upload završi */
        []() {
            wm.server.get()->send(200, "text/html; charset=utf-8",
                "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<style>"
                "body{font-family:Arial;padding:20px;max-width:500px;margin:auto;text-align:center}"
                ".msg{padding:20px;background:#d4edda;border-radius:5px;color:#155724}"
                "</style></head><body>"
                "<div class='msg'>"
                "<h3>Uspjesno ucitano!</h3>"
                "<p>Uredjaj se restartuje za 5 sekundi...</p>"
                "</div></body></html>");
            delay(5000);
            ESP.restart();
        },
        /* upload callback — prima svaki chunk multipart podataka */
        []() {
            HTTPUpload &upload = wm.server.get()->upload();
            static File s_upload_file;

            if (upload.status == UPLOAD_FILE_START) {
                LOG_INFO("[UPLOAD] Start: %s (%u B ukupno)",
                         upload.filename.c_str(), upload.totalSize);
                LittleFS.remove("/logo_480x100.png");
                s_upload_file = LittleFS.open("/logo_480x100.png", FILE_WRITE);
                if (!s_upload_file) {
                    LOG_ERROR("[UPLOAD] Ne mogu otvoriti fajl za pisanje!");
                } else {
                    LOG_INFO("[UPLOAD] Fajl otvoren za upis.");
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (s_upload_file) {
                    size_t written = s_upload_file.write(upload.buf, upload.currentSize);
                    if (written != upload.currentSize) {
                        LOG_ERROR("[UPLOAD] Greska pri upisu: written=%u / currentSize=%u",
                                  written, upload.currentSize);
                    }
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (s_upload_file) {
                    s_upload_file.close();
                    LOG_INFO("[UPLOAD] Zavrsen, ukupno: %u bajta", upload.totalSize);
                } else {
                    LOG_ERROR("[UPLOAD] Fajl nije bio otvoren na END!");
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                LOG_ERROR("[UPLOAD] Upload prekinut (ABORTED)!");
                if (s_upload_file) {
                    s_upload_file.close();
                    LittleFS.remove("/logo_480x100.png");
                }
            }
        }
    );

    LOG_INFO("[WiFi] Rute /upload i /upload_logo registrovane.");
}

static void wifi_manager_stop_portal() {
    if (!s_portal_running) return;
    
    // Only call stopConfigPortal if it is actually still active inside WiFiManager to avoid duplicate shutdown crashes
    if (wm.getConfigPortalActive()) {
        wm.stopConfigPortal();
    }
    
    WiFi.mode(WIFI_OFF);
    s_portal_running = false;
    g_wifi_ap_active = false;
    inactivity_reset();
    LOG_INFO("[WiFi] Config Portal stopped.");

    // Sync UI Switch if it exists
    if (ui_SwitchStartAp) {
        lv_obj_clear_state(ui_SwitchStartAp, LV_STATE_CHECKED);
    }
    
    // Apply DFS if the screen is currently in screensaver
    if (inactivity_is_screensaver_active()) {
        setCpuFrequencyMhz(80);
        LOG_INFO("[CFG] CPU clock reduced to 80MHz (Screensaver Active & WiFi Stopped)");
    }
}

void wifi_manager_tick() {
    if (s_portal_running) {
        wm.process(); // Handles the web server and timeout
        
        // Check if portal has timed out or closed
        if (wm.getConfigPortalActive() == false && s_portal_running) {
             LOG_INFO("[WiFi] AP Timeout or close from WiFiManager. Shutting down.");
             wifi_manager_stop_portal();
        }
    }
}

void wifi_manager_set_ap(bool enable) {
    if (enable && !s_portal_running) {
        LOG_INFO("[WiFi] Starting Config Portal...");
        setCpuFrequencyMhz(240);
        s_portal_running = true;
        g_wifi_ap_active = true; // Use existing global flag
        wm.setConfigPortalBlocking(false);
        wm.startConfigPortal("HotelThermostat", "12345678");
        LOG_INFO("[WiFi] AP started: HotelThermostat (Web Server on port 80)");
    } else if (!enable && s_portal_running) {
        wifi_manager_stop_portal();
    }
}

bool wifi_manager_has_credentials() {
    // ESP32 automatically stores WiFi credentials in NVS. 
    // If WiFi driver is off, WiFi.SSID() is empty.
    // We temporarily initialize STA mode to read the saved SSID from NVS.
    wifi_mode_t old_mode = WiFi.getMode();
    if (old_mode == WIFI_OFF || old_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    }
    
    bool has_creds = (WiFi.SSID().length() > 0) || (wm.getWiFiIsSaved());
    
    if (old_mode == WIFI_OFF || old_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
    }
    return has_creds;
}
