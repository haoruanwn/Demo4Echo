#ifndef _APP_WIFI_SERVICE_H
#define _APP_WIFI_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// 1. 用于扫描结果的 C 结构体
typedef struct {
    char bssid[32];
    int signal_level; // 信号强度 (dBm)
    char flags[128];    // 加密信息等, e.g., [WPA2-PSK]
    char ssid[64];      // SSID
} app_wifi_scan_result_t;

// 2. 用于连接状态的 C 枚举
typedef enum {
    APP_WIFI_STATUS_IDLE,
    APP_WIFI_STATUS_SCANNING,
    APP_WIFI_STATUS_SCAN_FINISHED,
    APP_WIFI_STATUS_CONNECTING,
    APP_WIFI_STATUS_CONNECTED,
    APP_WIFI_STATUS_DISCONNECTED,
    APP_WIFI_STATUS_CONNECTION_FAILED
} app_wifi_connection_status_t;


// --- 公开的 API 函数 ---

/**
 * @brief 初始化 Wi-Fi 服务, 启动 C++ 后端
 * @param iface_name 接口名称, e.g., "wlan0" (在模拟器模式下忽略)
 * @return 0 成功, -1 失败
 */
int app_wifi_init(const char *iface_name);

/**
 * @brief 停止 C++ 后端
 */
void app_wifi_deinit(void);

/**
 * @brief [核心] 轮询函数
 * 必须在 LVGL 的 lv_timer 中周期性调用
 * 它会触发 C++ 后端处理其内部事件队列
 */
void app_wifi_poll(void);

/**
 * @brief 异步请求 Wi-Fi 扫描
 */
void app_wifi_request_scan(void);

/**
 * @brief 异步请求连接到一个 Wi-Fi 网络
 * @param ssid SSID
 * @param psk 密码 (开放网络请传入 "" 或 NULL)
 */
void app_wifi_connect(const char *ssid, const char *psk);

/**
 * @brief 请求断开当前连接
 */
void app_wifi_disconnect(void);

/**
 * @brief [UI轮询] 检查是否有新的状态变化
 * @param out_status (出参) 如果有新状态, 写入此处
 * @param out_details (出参) 附加信息
 * @param details_len (入参) out_details 缓冲区的长度
 * @return true (有新状态), false (无新状态)
 */
bool app_wifi_get_new_status(app_wifi_connection_status_t* out_status, char* out_details, int details_len);

/**
 * @brief [UI轮询] 检查是否有新的扫描结果
 * @param out_results (出参) 结果数组
 * @param inout_count (入/出参) 传入时: 数组最大容量; 传出时: 实际结果数量
 * @return true (有新结果), false (无新结果)
 */
bool app_wifi_get_new_scan_results(app_wifi_scan_result_t* out_results, int* inout_count);


#ifdef __cplusplus
}
#endif

#endif // _APP_WIFI_SERVICE_H