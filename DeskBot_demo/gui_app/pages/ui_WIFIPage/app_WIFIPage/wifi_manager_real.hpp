#ifndef _WIFI_MANAGER_REAL_HPP
#define _WIFI_MANAGER_REAL_HPP

#include "iwifi_manager.hpp"
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include <queue>

// Fwd declaration
struct wpa_ctrl;

class RealWifiManager : public IWifiManager {
public:
    explicit RealWifiManager(const std::string& iface_name);
    virtual ~RealWifiManager();

    bool init() override;
    void deinit() override;
    void poll() override;

    void requestScan() override;
    void connect(const std::string& ssid, const std::string& psk) override;
    void disconnect() override;

    bool getNewStatus(WifiConnectionStatus& status, std::string& details) override;
    bool getNewScanResults(std::vector<WifiScanResult>& results) override;

private:
    // 监控线程循环
    void monitorLoop();
    
    // wpa_ctrl 命令辅助函数 (仅在主线程调用)
    int sendCmd(const char *cmd, char *reply_buf, size_t reply_buf_len);
    void fetchScanResults();

    std::string iface_name_; // 存储接口名称, e.g., "wlan0"
    std::string iface_path_; // e.g., "/var/run/wpa_supplicant/wlan0"
    struct wpa_ctrl *ctrl_cmd_ = nullptr;     // 用于主线程 (命令)
    struct wpa_ctrl *ctrl_monitor_ = nullptr; // 用于监控线程 (事件)

    // 线程管理
    std::thread monitorThread_;
    std::atomic<bool> monitorRunning_{false};

    // 内部状态 (由 'poll' 在主线程中更新)
    WifiConnectionStatus currentStatus_ = WifiConnectionStatus::IDLE;
    std::string statusDetails_;
    std::vector<WifiScanResult> lastScanResults_;
    
    // 状态标志 (由 'poll' 在主线程中设置, 由 getNew... 在主线程中消耗)
    std::atomic<bool> newStatusFlag_{false};
    std::atomic<bool> newScanFlag_{false};

    // 线程安全的事件队列 (Monitor -> Poll)
    enum class AppEvent {
        SCAN_FINISHED,
        CONNECTED,
        DISCONNECTED,
        AUTH_FAILED
    };
    std::queue<AppEvent> eventQueue_;
    std::mutex eventMutex_;
};

#endif // _WIFI_MANAGER_REAL_HPP