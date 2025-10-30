#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include "ThreadSafeQueue.h"
#include "WifiTypes.h"

struct RealWifiStrategy {
public:
    // Construction requires explicit paths; no defaults allowed.
    RealWifiStrategy(const std::string &ctrlPath, const std::string &ifaceName,
                     const std::string &wpaConfApp, const std::string &wpaConfDev);
    ~RealWifiStrategy();

    void RequestScan();
    void RequestConnect(const std::string &ssid, const std::string &psk);
    void RequestSwitchNetwork(bool toAppNetwork);

    std::optional<WifiScanResult> PollScanResult();
    std::optional<ConnectionStatus> PollConnectionStatus();

private:
    void WorkerLoop();
    void HandleWpaEvents();

    void DoScanRequest();
    void DoGetScanResults();
    void DoConnectRequest(const std::string &ssid, const std::string &psk);
    void DoSwitchNetwork(bool toAppNetwork);

    // internal state
    struct wpa_ctrl *m_ctrl_if = nullptr;
    struct wpa_ctrl *m_mon_if = nullptr;
    std::thread m_workerThread;
    std::atomic<bool> m_stopWorker{false};

    // Configurable paths / names (injected via ctor)
    std::string m_ctrl_path;
    std::string m_iface_name;
    std::string m_wpa_conf_app;
    std::string m_wpa_conf_dev;

    ThreadSafeQueue<std::function<void()>> m_taskQueue;
    ThreadSafeQueue<WifiScanResult> m_scanResultQueue;
    ThreadSafeQueue<ConnectionStatus> m_connectionStatusQueue;
};
