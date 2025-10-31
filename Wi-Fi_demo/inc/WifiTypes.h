#pragma once

#include <string>
#include <vector>

struct WifiScanResult {
    std::vector<std::string> ssids;
    bool operationSuccess = true;
    std::string errorMessage;
};

struct ConnectionStatus {
    bool isConnected = false;
    std::string ssid;
    // errorMessage is used for real errors. infoMessage is used for
    // informational or neutral status (e.g. "reused existing wpa_supplicant control socket").
    std::string errorMessage; // "密码错误", "无响应" 等
    std::string infoMessage;  // informational text, not an error
};
