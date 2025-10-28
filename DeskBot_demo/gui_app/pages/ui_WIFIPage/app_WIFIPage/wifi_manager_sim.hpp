// app/wifi_manager_sim.hpp
#ifndef _WIFI_MANAGER_SIM_HPP
#define _WIFI_MANAGER_SIM_HPP

#include "iwifi_manager.hpp"
#include <chrono>

class SimWifiManager : public IWifiManager {
public:
    SimWifiManager() {}
    virtual ~SimWifiManager() {}
    bool init() override { return true; }
    void deinit() override {}
    void poll() override;
    void requestScan() override;
    void connect(const std::string& ssid, const std::string& psk) override;
    void disconnect() override;
    bool getNewStatus(WifiConnectionStatus& status, std::string& details) override;
    bool getNewScanResults(std::vector<WifiScanResult>& results) override;

private:
    WifiConnectionStatus currentStatus_ = WifiConnectionStatus::IDLE;
    std::string statusDetails_;
    std::vector<WifiScanResult> lastScanResults_;
    bool newStatusFlag_ = false;
    bool newScanFlag_ = false;
    
    std::chrono::time_point<std::chrono::steady_clock> scanTimer_;
    std::chrono::time_point<std::chrono::steady_clock> connectTimer_;
    bool scanPending_ = false;
    bool connectPending_ = false;
};
#endif

