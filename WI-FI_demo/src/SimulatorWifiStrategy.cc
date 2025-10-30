#include "../inc/SimulatorWifiStrategy.h"

SimulatorWifiStrategy::SimulatorWifiStrategy() :
    m_stopWorker(false), m_worker(&SimulatorWifiStrategy::WorkerLoop, this) {}

SimulatorWifiStrategy::~SimulatorWifiStrategy() {
    m_stopWorker.store(true);
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
    }
    m_taskCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void SimulatorWifiStrategy::RequestScan() {
    auto task = [this]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        WifiScanResult fakeResult;
        fakeResult.ssids = {"SIM_WIFI_A", "SIM_WIFI_B (信号强)", "SIM_WIFI_C"};
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_scanResults.push(std::move(fakeResult));
    };
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        m_tasks.push(std::move(task));
    }
    m_taskCv.notify_one();
}

void SimulatorWifiStrategy::RequestConnect(const std::string &ssid, const std::string &psk) {
    auto task = [this, ssid, psk]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        ConnectionStatus st;
        st.ssid = ssid;
        if (psk == "password") {
            st.isConnected = true;
        } else {
            st.isConnected = false;
            st.errorMessage = "模拟：密码错误";
        }
        std::lock_guard<std::mutex> lk(m_resultMutex);
        m_connResults.push(std::move(st));
    };
    {
        std::lock_guard<std::mutex> lk(m_taskMutex);
        m_tasks.push(std::move(task));
    }
    m_taskCv.notify_one();
}

void SimulatorWifiStrategy::RequestSwitchNetwork(bool /*toAppNetwork*/) {
    // no-op for simulator
}

std::optional<WifiScanResult> SimulatorWifiStrategy::PollScanResult() {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    if (m_scanResults.empty())
        return std::nullopt;
    WifiScanResult r = std::move(m_scanResults.front());
    m_scanResults.pop();
    return r;
}

std::optional<ConnectionStatus> SimulatorWifiStrategy::PollConnectionStatus() {
    std::lock_guard<std::mutex> lk(m_resultMutex);
    if (m_connResults.empty())
        return std::nullopt;
    ConnectionStatus s = std::move(m_connResults.front());
    m_connResults.pop();
    return s;
}

void SimulatorWifiStrategy::WorkerLoop() {
    while (!m_stopWorker.load()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_taskMutex);
            m_taskCv.wait(lk, [this]() { return m_stopWorker.load() || !m_tasks.empty(); });
            if (m_stopWorker.load() && m_tasks.empty())
                break;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        try {
            if (task)
                task();
        } catch (...) {
            // ignore
        }
    }
}
