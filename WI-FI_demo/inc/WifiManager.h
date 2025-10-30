// 伪代码和类结构 (WifiManager.h)
#include "WifiStrategies.h"
#include <optional>
#include <future>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

// WifiManager 作为模板类，采用策略的鸭子类型。策略必须提供下面相同的接口：
//   void RequestScan();
//   void RequestConnect(const std::string&, const std::string&);
//   void RequestSwitchNetwork(bool);
//   std::optional<WifiScanResult> PollScanResult();
//   std::optional<ConnectionStatus> PollConnectionStatus();
//
// 设计选择：让策略实现自己的同步或异步细节（如 RealWifiStrategy 使用内部工作线程并
// 提供非阻塞的 Poll 接口），WifiManager 只负责作为轻量转发器并保持模板封装性。

template <typename WifiStrategy>
class WifiManager {
public:
    // 获取单例实例
    static WifiManager& GetInstance() {
        static WifiManager instance;
        return instance;
    }

    // 默认析构由类内部实现（用于停止 manager 线程）

    // --- 异步/同步请求接口 (由 LVGL 线程调用) ---
    // 直接转发给策略，由策略决定是否异步执行以及如何通过 Poll 接口回传结果。
    void RequestScan() {
        m_strategy.RequestScan();
    }

    void RequestConnect(const std::string& ssid, const std::string& psk) {
        m_strategy.RequestConnect(ssid, psk);
    }

    void RequestSwitchToAppNetwork() {
        m_strategy.RequestSwitchNetwork(true);
    }

    void RequestSwitchToDevNetwork() {
        m_strategy.RequestSwitchNetwork(false);
    }

    // --- 结果轮询接口 (由 LVGL 线程调用) ---
    std::optional<WifiScanResult> PollScanResult() {
        return m_strategy.PollScanResult();
    }

    std::optional<ConnectionStatus> PollConnectionStatus() {
        return m_strategy.PollConnectionStatus();
    }

    // --- Future/await 式异步 API (基于 C++17 promise/future)
    // 使用单一的 manager 线程聚合轮询（避免为每个请求创建线程）。
    // poll_interval: manager 轮询策略的间隔；timeout: 每个请求的超时时间。
    std::future<WifiScanResult> RequestScanAsync(
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max())
    {
        std::promise<WifiScanResult> prom;
        auto fut = prom.get_future();

        // 触发策略请求
        m_strategy.RequestScan();

        // 注册 pending entry
        PendingScan ps;
        ps.promise = std::move(prom);
        ps.startTime = std::chrono::steady_clock::now();
        ps.timeout = timeout;

        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingScans.push(std::move(ps));
        }
        m_pendingCv.notify_one();
        return fut;
    }

    std::future<ConnectionStatus> RequestConnectAsync(
        const std::string& ssid, const std::string& psk,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max())
    {
        std::promise<ConnectionStatus> prom;
        auto fut = prom.get_future();

        // 触发策略请求
        m_strategy.RequestConnect(ssid, psk);

        PendingConnect pc;
        pc.promise = std::move(prom);
        pc.startTime = std::chrono::steady_clock::now();
        pc.timeout = timeout;

        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            m_pendingConnects.push(std::move(pc));
        }
        m_pendingCv.notify_one();
        return fut;
    }

private:
    // Pending entries for manager thread
    struct PendingScan {
        std::promise<WifiScanResult> promise;
        std::chrono::steady_clock::time_point startTime;
        std::chrono::milliseconds timeout;
    };

    struct PendingConnect {
        std::promise<ConnectionStatus> promise;
        std::chrono::steady_clock::time_point startTime;
        std::chrono::milliseconds timeout;
    };

    // manager thread: 聚合轮询并满足 promise
    void FutureManagerLoop() {
        const std::chrono::milliseconds poll_interval(100);
        while (!m_stopManager.load()) {
            // 1) check for immediate results from strategy
            // handle scan results
            while (true) {
                auto opt = m_strategy.PollScanResult();
                if (!opt) break;
                PendingScan pending;
                {
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    if (m_pendingScans.empty()) {
                        // no pending promise: drop the result
                        continue;
                    }
                    pending = std::move(m_pendingScans.front());
                    m_pendingScans.pop();
                }
                // 满足 promise（使用返回值表示成功）
                pending.promise.set_value(std::move(*opt));
            }

            // handle connect results
            while (true) {
                auto opt = m_strategy.PollConnectionStatus();
                if (!opt) break;
                PendingConnect pending;
                {
                    std::lock_guard<std::mutex> lk(m_pendingMutex);
                    if (m_pendingConnects.empty()) {
                        // no pending promise: drop the result
                        continue;
                    }
                    pending = std::move(m_pendingConnects.front());
                    m_pendingConnects.pop();
                }
                pending.promise.set_value(std::move(*opt));
            }

            // 2) handle timeouts
            {
                std::lock_guard<std::mutex> lk(m_pendingMutex);
                // scans
                size_t scanCount = m_pendingScans.size();
                for (size_t i = 0; i < scanCount; ++i) {
                    PendingScan pending = std::move(m_pendingScans.front());
                    m_pendingScans.pop();
                    auto now = std::chrono::steady_clock::now();
                    if (pending.timeout != std::chrono::milliseconds::max() &&
                        now - pending.startTime >= pending.timeout) {
                        WifiScanResult r;
                        r.operationSuccess = false;
                        r.errorMessage = "timeout";
                        pending.promise.set_value(std::move(r));
                    } else {
                        // not timed out yet, requeue
                        m_pendingScans.push(std::move(pending));
                    }
                }

                // connects
                size_t connCount = m_pendingConnects.size();
                for (size_t i = 0; i < connCount; ++i) {
                    PendingConnect pending = std::move(m_pendingConnects.front());
                    m_pendingConnects.pop();
                    auto now = std::chrono::steady_clock::now();
                    if (pending.timeout != std::chrono::milliseconds::max() &&
                        now - pending.startTime >= pending.timeout) {
                        ConnectionStatus s;
                        s.isConnected = false;
                        s.errorMessage = "timeout";
                        pending.promise.set_value(std::move(s));
                    } else {
                        m_pendingConnects.push(std::move(pending));
                    }
                }
            }

            // wait for notification or timeout interval
            std::unique_lock<std::mutex> lk(m_pendingMutex);
            m_pendingCv.wait_for(lk, poll_interval, [this]() {
                return m_stopManager.load() || !m_pendingScans.empty() || !m_pendingConnects.empty();
            });
        }

        // On shutdown, fulfill all pending promises with failure results
        {
            std::lock_guard<std::mutex> lk(m_pendingMutex);
            while (!m_pendingScans.empty()) {
                auto pending = std::move(m_pendingScans.front());
                m_pendingScans.pop();
                WifiScanResult r;
                r.operationSuccess = false;
                r.errorMessage = "shutdown";
                pending.promise.set_value(std::move(r));
            }
            while (!m_pendingConnects.empty()) {
                auto pending = std::move(m_pendingConnects.front());
                m_pendingConnects.pop();
                ConnectionStatus s;
                s.isConnected = false;
                s.errorMessage = "shutdown";
                pending.promise.set_value(std::move(s));
            }
        }
    }

    // manager members
    std::atomic<bool> m_stopManager{false};
    std::thread m_managerThread;
    std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    std::queue<PendingScan> m_pendingScans;
    std::queue<PendingConnect> m_pendingConnects;
public:
    // start manager thread in constructor and stop in destructor
    WifiManager() : m_managerThread(&WifiManager::FutureManagerLoop, this) {}
    ~WifiManager() {
        m_stopManager.store(true);
        m_pendingCv.notify_all();
        if (m_managerThread.joinable()) m_managerThread.join();
    }
    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

    WifiStrategy m_strategy; // 策略实例 (Real 或 Simulator)
};