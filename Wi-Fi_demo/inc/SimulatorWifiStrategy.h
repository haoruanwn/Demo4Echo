#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "WifiTypes.h"

struct SimulatorWifiStrategy {
public:
    SimulatorWifiStrategy();
    ~SimulatorWifiStrategy();

    // non-blocking API: enqueue a task and return immediately
    void RequestScan();
    void RequestConnect(const std::string &ssid, const std::string &psk);
    void RequestSwitchNetwork(bool toAppNetwork);

    // non-blocking poll for results
    std::optional<WifiScanResult> PollScanResult();
    std::optional<ConnectionStatus> PollConnectionStatus();

private:
    void WorkerLoop();

    std::atomic<bool> m_stopWorker;
    std::thread m_worker;

    std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    std::queue<std::function<void()>> m_tasks;

    // results queues
    std::mutex m_resultMutex;
    std::queue<WifiScanResult> m_scanResults;
    std::queue<ConnectionStatus> m_connResults;
};
