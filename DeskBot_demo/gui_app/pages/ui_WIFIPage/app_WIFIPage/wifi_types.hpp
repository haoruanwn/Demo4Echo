#ifndef _WIFI_TYPES_HPP
#define _WIFI_TYPES_HPP

#include <string>
#include <vector>

// 2. 用于连接状态的 C++ 枚举
enum class WifiConnectionStatus {
    IDLE,
    SCANNING,
    SCAN_FINISHED,
    CONNECTING,
    CONNECTED,
    DISCONNECTED,
    CONNECTION_FAILED
};

// 1. 用于扫描结果的 C++ 结构体
struct WifiScanResult {
    std::string bssid;
    int signal_level; // 信号强度 (dBm)
    std::string flags;    // 加密信息等, e.g., [WPA2-PSK]
    std::string ssid;     // SSID
};

#endif // _WIFI_TYPES_HPP