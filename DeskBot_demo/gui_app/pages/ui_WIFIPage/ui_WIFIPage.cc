// C++ rewrite of ui_WIFIPage using WIFI_demo library (SimulatorWifiStrategy)
// This file replaces the previous C implementation and directly uses the
// WifiManager template and strategy from the Wi-Fi_demo headers.

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include "ui_WIFIPage.h"
}

// Include Wi-Fi demo headers (relative path from this file). We use Simulator
// strategy to avoid requiring system wpa_supplicant paths at runtime.
#include "../../../../Wi-Fi_demo/inc/WifiManager.h"
#include "../../../../Wi-Fi_demo/inc/SimulatorWifiStrategy.h"
#include "../../../../Wi-Fi_demo/inc/RealWifiStrategy.h"
#include "../../../../Wi-Fi_demo/inc/WifiTypes.h"
#include "../../conf/dev_conf.h"  // for LV_USE_SIMULATOR

using namespace std;

#if defined(LV_USE_SIMULATOR) && (LV_USE_SIMULATOR == 1)
    // 编译时选择模拟器策略
    using MyWifiManager = WifiManager<SimulatorWifiStrategy>;
#else
    // 编译时选择真实策略
    using MyWifiManager = WifiManager<RealWifiStrategy>;
#endif

// 指向单例的裸指针（不要 delete），由 WifiManager::GetInstance 管理生命周期
static MyWifiManager *s_wifiManager = nullptr;

// Local UI globals (kept same names as original for compatibility)
lv_obj_t * ui_WIFIRootMenu = nullptr;
lv_obj_t * ui_WIFIList = nullptr;
lv_obj_t * ui_BtnScan = nullptr;
lv_obj_t * ui_SpinnerScan = nullptr;

// 在这里设置配置文件、socket路径等参数
std::string ctrlPath = "/var/run/wpa_supplicant";
std::string iface = "wlan0";
std::string wpa_conf_app = "/etc/wpa_supplicant_app.conf";
std::string wpa_conf_dev = "/etc/wpa_supplicant_dev.conf";

// Helper: check whether a path exists and is a socket
static bool is_socket(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISSOCK(st.st_mode);
}

// Try common control directories and check for iface socket
static std::string detect_ctrl_dir_for_iface(const std::string &iface) {
    const char *candidates[] = {"/var/run/wpa_supplicant", "/run/wpa_supplicant", "/var/run"};
    for (const char *base : candidates) {
        std::string candidate = std::string(base) + "/" + iface;
        if (is_socket(candidate)) return std::string(base);
    }
    return std::string();
}

// 存储选中的 SSID  
static char selected_ssid[64];
lv_obj_t * ui_TextAreaPassword = nullptr;
lv_obj_t * ui_Keyboard = nullptr;

static lv_timer_t * wifi_poll_timer = nullptr;
static lv_obj_t * ui_ConnectingModal = nullptr;
static lv_obj_t * ui_PasswordModal = nullptr;

// Forward declarations for static callbacks
static void wifi_ssid_click_cb(lv_event_t * e);
static void wifi_main_back_event_handler(lv_event_t * e);
static void show_password_modal(void);
static void hide_password_modal(void);

// Helper: show connecting modal
static void ui_show_connecting_modal(void)
{
    if (ui_ConnectingModal) {
        lv_obj_del(ui_ConnectingModal);
        ui_ConnectingModal = nullptr;
    }
    lv_obj_t * mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    ui_ConnectingModal = lv_obj_create(mask);
    lv_obj_set_size(ui_ConnectingModal, 200, 100);
    lv_obj_align(ui_ConnectingModal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ui_ConnectingModal, lv_color_white(), 0);
    lv_obj_set_style_radius(ui_ConnectingModal, 8, 0);

    lv_obj_t * spinner = lv_spinner_create(ui_ConnectingModal);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t * label = lv_label_create(ui_ConnectingModal);
    lv_label_set_text(label, "Connecting...");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 30);

    ui_ConnectingModal = mask;
}

// Poll helpers: check wifi manager for new scan results and connection status
static void ui_update_scan_list(void)
{
    if (lv_scr_act() != ui_WIFIRootMenu) return;

    if (!s_wifiManager) return;
    auto opt = s_wifiManager->PollScanResult();
    if (!opt) return;

    const WifiScanResult &r = *opt;
    lv_obj_clean(ui_WIFIList);
    if (r.ssids.empty()) {
        lv_list_add_text(ui_WIFIList, "No networks found");
    } else {
        for (const auto &ssid : r.ssids) {
            // Ignore empty or placeholder names
            if (ssid.empty()) continue;
            lv_obj_t * btn = lv_list_add_btn(ui_WIFIList, LV_SYMBOL_WIFI, ssid.c_str());
            // Use the same C callback; we don't need per-button user_data because
            // lv_list_get_btn_text can fetch the label text.
            lv_obj_add_event_cb(btn, wifi_ssid_click_cb, LV_EVENT_CLICKED, nullptr);
        }
    }

    // hide spinner and re-enable scan button
    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(ui_BtnScan, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scan");
}

static void ui_update_wifi_status(void)
{
    if (!s_wifiManager) return;
    auto opt = s_wifiManager->PollConnectionStatus();
    if (!opt) return;

    const ConnectionStatus &s = *opt;

    // connection finished (either success or any message) should dismiss modal
    if (s.isConnected || (!s.isConnected && (!s.errorMessage.empty() || !s.infoMessage.empty()))) {
        if (ui_ConnectingModal) {
            lv_obj_del(ui_ConnectingModal);
            ui_ConnectingModal = nullptr;
        }
    }

    if (s.isConnected) {
        ui_msgbox_info("Success", "Wi-Fi Connected!");
        // Exit Wi-Fi app (same behaviour as original)
        wifi_main_back_event_handler(nullptr);
    } else if (!s.errorMessage.empty()) {
        ui_msgbox_info("Error", s.errorMessage.c_str());
    } else if (!s.infoMessage.empty()) {
        ui_msgbox_info("Info", s.infoMessage.c_str());
    }
}

// LVGL timer callback: drive wifi manager polling and UI updates
static void wifi_poll_timer_cb(lv_timer_t * timer)
{
    // Let manager do internal work (if any) - WifiManager doesn't expose poll, but
    // our strategy may need nothing here; still, keep for future changes.
    // s_wifiManager->m_strategy... (not needed)

    // check updates
    ui_update_wifi_status();
    ui_update_scan_list();
}

// Event handlers
static void wifi_main_back_event_handler(lv_event_t * e)
{
    LV_LOG_USER("Exiting Wi-Fi, de-initializing WIFI_demo manager.");

    if (wifi_poll_timer) {
        lv_timer_del(wifi_poll_timer);
        wifi_poll_timer = nullptr;
    }

    // Note: do NOT stop or switch the global WifiManager here. The manager
    // lifecycle should be bound to the whole executable (app-level), not the
    // UI page. Exiting the Wi‑Fi page must not trigger network switching or
    // destruct the manager. The global manager should be created at program
    // init (see suggestions below).

    // call original page manager back
    lv_lib_pm_OpenPrePage(&page_manager);
}

static void back_event_handler(lv_event_t * e)
{
    lv_lib_pm_OpenPrePage(&page_manager);
}

static void scan_button_event_handler(lv_event_t * e)
{
    if (e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code != LV_EVENT_CLICKED) return;
    }

    LV_LOG_USER("Wi-Fi scan triggered (C++ backend).");
    lv_obj_clean(ui_WIFIList);
    lv_list_add_text(ui_WIFIList, "Scanning...");
    lv_obj_add_state(ui_BtnScan, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scanning...");
    lv_obj_clear_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);

    if (s_wifiManager) {
        s_wifiManager->RequestScan();
    } else {
        LV_LOG_ERROR("wifi manager not initialized, can't RequestScan");
    }
}

static void wifi_ssid_click_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    const char * ssid = lv_list_get_btn_text(ui_WIFIList, btn);

    if (code == LV_EVENT_CLICKED) {
        if (!ssid) return;
        if (strcmp(ssid, "Scanning...") == 0 || strcmp(ssid, "No networks found") == 0) return;

        LV_LOG_USER("Selected SSID: %s", ssid);
        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        selected_ssid[sizeof(selected_ssid) - 1] = '\0';

    // Show password input as a modal overlay so the root page is not deinitialized.
    show_password_modal();
    }
}

static void keyboard_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        const char * password = lv_textarea_get_text(ui_TextAreaPassword);
        LV_LOG_USER("Attempting to connect to %s (C++ backend)", selected_ssid);
        ui_show_connecting_modal();
        if (s_wifiManager) {
            s_wifiManager->RequestConnect(std::string(selected_ssid), std::string(password ? password : ""));
        } else {
            LV_LOG_ERROR("wifi manager not initialized, can't RequestConnect");
        }
    } else if (code == LV_EVENT_CANCEL) {
        // Hide our modal overlay (no page-manager navigation)
        hide_password_modal();
    }
}

// Textarea key event callback: treat Enter key as submit so virtual keyboard
// Enter and hardware keyboard Enter both trigger connect immediately.
static void textarea_key_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY) {
        int key = lv_event_get_key(e);
        if (key == LV_KEY_ENTER) {
            const char * password = lv_textarea_get_text(ui_TextAreaPassword);
            LV_LOG_USER("Attempting to connect to %s (C++ backend via Enter)", selected_ssid);
            ui_show_connecting_modal();
            if (s_wifiManager) {
                s_wifiManager->RequestConnect(std::string(selected_ssid), std::string(password ? password : ""));
            } else {
                LV_LOG_ERROR("wifi manager not initialized, can't RequestConnect");
            }
        }
    }
}

// Subscreen: password menu init/deinit (ported from C)
// Instead of using the page manager for password input (which will deinit
// the Wi-Fi root page and cause WifiManager lifecycle issues), implement a
// modal overlay password dialog that does not replace the root page.
static void hide_password_modal(void)
{
    if (ui_PasswordModal) {
        lv_obj_del(ui_PasswordModal);
        ui_PasswordModal = nullptr;
    }
    ui_TextAreaPassword = nullptr;
    ui_Keyboard = nullptr;
    LV_LOG_USER("Password modal hidden");
}

static void show_password_modal(void)
{
    // If already shown, refresh SSID label
    if (ui_PasswordModal) return;

    LV_LOG_USER("Showing password modal for SSID: %s", selected_ssid);

    lv_obj_t * mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * modal = lv_obj_create(mask);
    // Make modal taller so keyboard fits without overlapping the password field
    lv_obj_set_size(modal, 320, 300);
    lv_obj_align(modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal, lv_color_white(), 0);
    lv_obj_set_style_radius(modal, 8, 0);

    lv_obj_t * title = lv_label_create(modal);
    lv_label_set_text_fmt(title, "SSID: %s", selected_ssid);
    lv_obj_set_align(title, LV_ALIGN_TOP_MID);
    lv_obj_set_y(title, 8);

    ui_TextAreaPassword = lv_textarea_create(modal);
    lv_obj_set_width(ui_TextAreaPassword, 280);
    lv_obj_set_height(ui_TextAreaPassword, 40);
    // position password field just under title with a small margin
    lv_obj_align(ui_TextAreaPassword, LV_ALIGN_TOP_MID, 0, 40);
    lv_textarea_set_placeholder_text(ui_TextAreaPassword, "Password...");
    lv_textarea_set_one_line(ui_TextAreaPassword, true);
    lv_textarea_set_password_mode(ui_TextAreaPassword, true);
    // Handle Enter from hardware or virtual keyboard by listening to LV_EVENT_KEY
    lv_obj_add_event_cb(ui_TextAreaPassword, [](lv_event_t * e){ textarea_key_event_cb(e); }, LV_EVENT_KEY, NULL);

    ui_Keyboard = lv_keyboard_create(modal);
    // Make keyboard slightly larger and anchor to bottom of modal so it does
    // not overlap the password field.
    lv_obj_set_size(ui_Keyboard, 300, 160);
    lv_obj_align(ui_Keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaPassword);
    lv_obj_add_event_cb(ui_Keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // Close button: place at top-right inside modal so it doesn't cover the
    // keyboard enter key.
    lv_obj_t * btnClose = lv_btn_create(modal);
    lv_obj_set_size(btnClose, 60, 36);
    lv_obj_align(btnClose, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_t * lblClose = lv_label_create(btnClose);
    lv_label_set_text(lblClose, "Close");
    lv_obj_add_event_cb(btnClose, [](lv_event_t * e){ hide_password_modal(); }, LV_EVENT_CLICKED, NULL);

    ui_PasswordModal = mask;
}

// Password subpage is implemented as a modal overlay in this C++ port; we
// do not register it with the page manager to avoid deinitializing the
// Wi-Fi root page when the user opens the password input.

// 该函数作为WIFIPage的入口函数，负责初始化UI和WifiManager实例
// Screen init
void ui_WIFIPage_init(void)
{
    static bool inited = false;
    if (!inited) {
        // No sub-pages to register (password dialog is modal). Mark as inited.
        inited = true;
    }

    ui_WIFIRootMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIRootMenu, LV_OBJ_FLAG_SCROLLABLE);

    // Back button bound to cleanup handler
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIRootMenu);
    lv_obj_set_width(ui_BtnBack, 50);
    lv_obj_set_height(ui_BtnBack, 45);
    lv_obj_set_x(ui_BtnBack, 5);
    lv_obj_set_y(ui_BtnBack, 0);
    lv_obj_set_style_bg_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BtnBack, 64, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_BtnBack, wifi_main_back_event_handler, LV_EVENT_CLICKED, ui_WIFIRootMenu);

    lv_obj_t * ui_LabelBack = lv_label_create(ui_BtnBack);
    lv_label_set_text(ui_LabelBack, "");
    lv_obj_set_style_text_font(ui_LabelBack, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Scan button
    ui_BtnScan = lv_button_create(ui_WIFIRootMenu);
    lv_obj_set_width(ui_BtnScan, 80);
    lv_obj_set_height(ui_BtnScan, 40);
    lv_obj_set_align(ui_BtnScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_BtnScan, -10);
    lv_obj_set_y(ui_BtnScan, 5);
    lv_obj_add_event_cb(ui_BtnScan, [](lv_event_t * e){ scan_button_event_handler(e); }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * ui_LabelScan = lv_label_create(ui_BtnScan);
    lv_label_set_text(ui_LabelScan, "Scan");
    lv_obj_set_align(ui_LabelScan, LV_ALIGN_CENTER);

    // Spinner
    ui_SpinnerScan = lv_spinner_create(ui_WIFIRootMenu);
    lv_obj_set_size(ui_SpinnerScan, 24, 24);
    lv_obj_set_align(ui_SpinnerScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_SpinnerScan, -100);
    lv_obj_set_y(ui_SpinnerScan, 13);
    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);

    // List
    ui_WIFIList = lv_list_create(ui_WIFIRootMenu);
    lv_obj_set_size(ui_WIFIList, 310, 180);
    lv_obj_set_align(ui_WIFIList, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(ui_WIFIList, -5);

    // Create wifi manager instance (Simulator strategy). If later you want Real,
    // swap template arg and provide appropriate ctor args.
    // s_wifiManager = make_unique<MyWifiManager>();
    // 构造 WifiManager 实例，传入文件路径参数

    // Bind/create the WifiManager singleton. Only request a network-model
    // switch when we did not previously have a bound manager (i.e. first
    // time the Wi-Fi page is entered).
    bool need_switch_to_app = (s_wifiManager == nullptr);

#if defined(LV_USE_SIMULATOR) && (LV_USE_SIMULATOR == 1)
    s_wifiManager = &MyWifiManager::GetInstance();
#else
    s_wifiManager = &MyWifiManager::GetInstance(
        ctrlPath,
        iface,
        wpa_conf_app,
        wpa_conf_dev
    );
#endif

    if (need_switch_to_app && s_wifiManager) {
        s_wifiManager->RequestSwitchToAppNetwork();
    }

    // Create poll timer (ensure single)
    if (wifi_poll_timer) {
        lv_timer_del(wifi_poll_timer);
    }
    wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, nullptr);

    // Initial scan (only request when manager exists)
    if (s_wifiManager) scan_button_event_handler(nullptr);

    lv_scr_load_anim(ui_WIFIRootMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}

void ui_WIFIPage_deinit()
{
    LV_LOG_USER("ui_WIFIPage_deinit: (Skipping timer/service deinit here)");
    ui_WIFIRootMenu = nullptr;
    ui_WIFIList = nullptr;
    ui_BtnScan = nullptr;
    ui_SpinnerScan = nullptr;
}
