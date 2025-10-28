#ifndef _IWIFI_MANAGER_HPP
#define _IWIFI_MANAGER_HPP

#include "wifi_types.hpp"
#include <memory>
#include <string>

class IWifiManager {
public:
    virtual ~IWifiManager() = default;

    virtual bool init() = 0;
    virtual void deinit() = 0;

    // 轮询: 让管理器处理内部事件 (例如从监控线程接收)
    virtual void poll() = 0;

    // --- 命令 (从主线程调用) ---
    virtual void requestScan() = 0;
    virtual void connect(const std::string& ssid, const std::string& psk) = 0;
    virtual void disconnect() = 0;

    // --- 状态查询 (从主线程调用) ---
    // 检查是否有新状态, 如果有, 则通过引用返回
    virtual bool getNewStatus(WifiConnectionStatus& status, std::string& details) = 0;
    
    // 检查是否有新扫描结果, 如果有, 则返回拷贝
    virtual bool getNewScanResults(std::vector<WifiScanResult>& results) = 0;
};

#endif // _IWIFI_MANAGER_HPP