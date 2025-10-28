#include "./inc/WifiManager.h"
#include <cstring>

// C 结构体，用于在C代码中传递扫描结果
typedef struct {
    char ssid[64];
    int signal;
} CWifiScanResult;

// 声明C回调函数类型
typedef void (*scan_finished_cb_t)(CWifiScanResult* results, int count);
typedef void (*status_changed_cb_t)(int event_type); // 0=connected, 1=disconnected

// 全局回调
static scan_finished_cb_t g_scan_cb = nullptr;
static status_changed_cb_t g_status_cb = nullptr;

// LVGL 调用的事件处理器
static void lvgl_event_handler(WifiEvent event, void* data) {
    if (event == WifiEvent::ScanFinished) {
        if (g_scan_cb && data) {
            auto* results_vec = static_cast<std::vector<WifiScanResult>*>(data);
            
            // 转换为C结构体数组 (简化版)
            std::vector<CWifiScanResult> c_results;
            for (const auto& res : *results_vec) {
                CWifiScanResult c_res;
                strncpy(c_res.ssid, res.ssid.c_str(), 63);
                c_res.ssid[63] = '\0';
                c_res.signal = res.signal_level;
                c_results.push_back(c_res);
            }
            
            // 调用C回调
            g_scan_cb(c_results.data(), c_results.size());
            
            // 释放C++中new的内存
            delete results_vec;
        }
    } else if (event == WifiEvent::Connected) {
        if (g_status_cb) g_status_cb(0);
    } else if (event == WifiEvent::Disconnected) {
        if (g_status_cb) g_status_cb(1);
    }
    // ...
}

extern "C" {
    // 我们的C++对象
    WifiManager* g_wifi_manager = nullptr;
    
    // C API: 初始化
    void wifi_service_init(const char* iface) {
        if (g_wifi_manager == nullptr) {
            g_wifi_manager = new WifiManager(iface);
            if (!g_wifi_manager->Start()) {
                delete g_wifi_manager;
                g_wifi_manager = nullptr;
            }
        }
    }
    
    // C API: 停止
    void wifi_service_deinit() {
        if (g_wifi_manager) {
            delete g_wifi_manager;
            g_wifi_manager = nullptr;
        }
    }
    
    // C API: 注册回调
    void wifi_service_register_scan_cb(scan_finished_cb_t cb) {
        g_scan_cb = cb;
    }
    void wifi_service_register_status_cb(status_changed_cb_t cb) {
        g_status_cb = cb;
    }

    // C API: LVGL 定时器调用的轮询函数
    void wifi_service_poll() {
        if (g_wifi_manager) {
            g_wifi_manager->PollEvents(lvgl_event_handler);
        }
    }
    
    // C API: 触发扫描
    void wifi_service_request_scan() {
        if (g_wifi_manager) {
            g_wifi_manager->RequestScan();
        }
    }
    
    // C API: 触发连接
    void wifi_service_connect(const char* ssid, const char* psk) {
        if (g_wifi_manager) {
            g_wifi_manager->Connect(ssid, psk);
        }
    }
}