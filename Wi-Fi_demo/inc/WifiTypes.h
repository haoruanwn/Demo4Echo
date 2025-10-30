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
    std::string errorMessage; // "密码错误", "无响应" 等
};
