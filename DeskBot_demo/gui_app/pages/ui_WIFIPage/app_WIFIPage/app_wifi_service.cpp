#include "app_wifi_service.h"
#include "wifi_service.hpp" // C++ Singleton
#include "wifi_types.hpp"
#include <cstring>

// [核心] 根据 LV_USE_SIMULATOR 包含不同的 C++ 实现
#if LV_USE_SIMULATOR == 1
    #include "wifi_manager_sim.hpp"
#else
    #include "wifi_manager_real.hpp"
#endif

// 辅助函数: C++ enum -> C enum
static app_wifi_connection_status_t CppStatusToC(WifiConnectionStatus cpp_status) {
    switch(cpp_status) {
        case WifiConnectionStatus::IDLE:            return APP_WIFI_STATUS_IDLE;
        case WifiConnectionStatus::SCANNING:        return APP_WIFI_STATUS_SCANNING;
        case WifiConnectionStatus::SCAN_FINISHED:   return APP_WIFI_STATUS_SCAN_FINISHED;
        case WifiConnectionStatus::CONNECTING:      return APP_WIFI_STATUS_CONNECTING;
        case WifiConnectionStatus::CONNECTED:       return APP_WIFI_STATUS_CONNECTED;
        case WifiConnectionStatus::DISCONNECTED:    return APP_WIFI_STATUS_DISCONNECTED;
        case WifiConnectionStatus::CONNECTION_FAILED: return APP_WIFI_STATUS_CONNECTION_FAILED;
        default:                                    return APP_WIFI_STATUS_IDLE;
    }
}

// 辅助函数: C++ struct -> C struct
static void CppResultToC(const WifiScanResult& cpp_res, app_wifi_scan_result_t* c_res) {
    strncpy(c_res->bssid, cpp_res.bssid.c_str(), sizeof(c_res->bssid) - 1);
    c_res->bssid[sizeof(c_res->bssid) - 1] = '\0';
    
    strncpy(c_res->flags, cpp_res.flags.c_str(), sizeof(c_res->flags) - 1);
    c_res->flags[sizeof(c_res->flags) - 1] = '\0';
    
    strncpy(c_res->ssid, cpp_res.ssid.c_str(), sizeof(c_res->ssid) - 1);
    c_res->ssid[sizeof(c_res->ssid) - 1] = '\0';
    
    c_res->signal_level = cpp_res.signal_level;
}


// --- C API 实现 ---

int app_wifi_init(const char *iface_name) {
    // [核心] 在这里切换实现
#if LV_USE_SIMULATOR == 1
    WifiService::getInstance().setManager(std::make_unique<SimWifiManager>());
#else
    WifiService::getInstance().setManager(std::make_unique<RealWifiManager>(iface_name ? iface_name : "wlan0"));
#endif

    if (WifiService::getInstance().getManager()->init()) {
        return 0;
    } else {
        return -1;
    }
}

void app_wifi_deinit(void) {
    WifiService::getInstance().deinit(); // deinit 会自动销毁 manager
}

void app_wifi_poll(void) {
    auto manager = WifiService::getInstance().getManager();
    if(manager) {
        manager->poll();
    }
}

void app_wifi_request_scan(void) {
    auto manager = WifiService::getInstance().getManager();
    if(manager) {
        manager->requestScan();
    }
}

void app_wifi_connect(const char *ssid, const char *psk) {
    auto manager = WifiService::getInstance().getManager();
    if(manager) {
        manager->connect(ssid ? ssid : "", psk ? psk : "");
    }
}

void app_wifi_disconnect(void) {
    auto manager = WifiService::getInstance().getManager();
    if(manager) {
        manager->disconnect();
    }
}

bool app_wifi_get_new_status(app_wifi_connection_status_t* out_status, char* out_details, int details_len) {
    auto manager = WifiService::getInstance().getManager();
    if(!manager) return false;

    WifiConnectionStatus cpp_status;
    std::string cpp_details;
    
    if (manager->getNewStatus(cpp_status, cpp_details)) {
        *out_status = CppStatusToC(cpp_status);
        strncpy(out_details, cpp_details.c_str(), details_len - 1);
        out_details[details_len - 1] = '\0';
        return true;
    }
    return false;
}

bool app_wifi_get_new_scan_results(app_wifi_scan_result_t* out_results, int* inout_count) {
    auto manager = WifiService::getInstance().getManager();
    if(!manager || !out_results || !inout_count || *inout_count == 0) return false;

    std::vector<WifiScanResult> cpp_results;
    if (manager->getNewScanResults(cpp_results)) {
        int max_count = *inout_count;
        int copied_count = 0;
        
        for (const auto& cpp_res : cpp_results) {
            if (copied_count >= max_count) break;
            CppResultToC(cpp_res, &out_results[copied_count]);
            copied_count++;
        }
        
        *inout_count = copied_count;
        return true;
    }
    
    return false;
}