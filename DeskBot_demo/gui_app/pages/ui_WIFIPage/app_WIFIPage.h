#ifndef _WIFI_SERVICE_H
#define _WIFI_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

// 1. 用于扫描结果的 C 结构体
typedef struct {
    char bssid[32];
    int signal_level; // 信号强度 (dBm)
    char flags[128];    // 加密信息等, e.g., [WPA2-PSK]
    char ssid[64];      // SSID
} wifi_scan_result_t;

// 2. 用于连接状态的 C 枚举
typedef enum {
    WIFI_STATUS_IDLE,
    WIFI_STATUS_SCANNING,
    WIFI_STATUS_SCAN_FINISHED,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_CONNECTION_FAILED
} wifi_connection_status_t;

// 3. LVGL (C) 可以注册的回调函数
/**
 * @brief 扫描完成时调用的回调
 * @param results 扫描结果数组
 * @param count 数组中的项数
 */
typedef void (*wifi_scan_cb_t)(wifi_scan_result_t *results, int count);

/**
 * @brief Wi-Fi 连接状态改变时调用的回调
 * @param status 新的状态
 * @param details (可选) 附加信息, 如 "WRONG_PASSWORD"
 */
typedef void (*wifi_status_cb_t)(wifi_connection_status_t status, const char *details);


// --- 公开的 API 函数 ---

/**
 * @brief 初始化 Wi-Fi 服务, 打开 wpa_ctrl, 并启动监视器线程
 * @param iface_name 接口名称, e.g., "wlan0"
 * @return 0 成功, -1 失败
 */
int wifi_service_init(const char *iface_name);

/**
 * @brief 停止监视器线程, 关闭 wpa_ctrl 连接
 */
void wifi_service_deinit(void);

/**
 * @brief 注册 LVGL 的回调函数
 * @param scan_cb 扫描完成回调
 * @param status_cb 状态变化回调
 */
void wifi_service_register_callbacks(wifi_scan_cb_t scan_cb, wifi_status_cb_t status_cb);

/**
 * @brief 异步请求 Wi-Fi 扫描
 * (在 LVGL 线程中调用)
 * 结果将通过 'scan_cb' 返回
 */
void wifi_service_request_scan(void);

/**
 * @brief 异步请求连接到一个 Wi-Fi 网络
 * (在 LVGL 线程中调用)
 * 结果将通过 'status_cb' 返回
 * @param ssid SSID
 * @param psk 密码 (开放网络请传入 "" 或 NULL)
 */
void wifi_service_connect(const char *ssid, const char *psk);

/**
 * @brief 请求断开当前连接
 */
void wifi_service_disconnect(void);

/**
 * @brief [核心] LVGL 轮询函数
 * 必须在 LVGL 的 lv_timer 中周期性调用 (e.g., 100ms)
 * 它负责检查来自监视器线程的事件, 并在 LVGL 线程中执行回调
 */
void wifi_service_poll(void);


#ifdef __cplusplus
}
#endif

#endif // _WIFI_SERVICE_H