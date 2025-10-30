#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <utility>
#include "WifiTypes.h"


// WifiManager 作为模板类，采用策略的鸭子类型。策略必须提供下面相同的接口：
//   void RequestScan();
//   void RequestConnect(const std::string&, const std::string&);
//   void RequestSwitchNetwork(bool);
//   std::optional<WifiScanResult> PollScanResult();
//   std::optional<ConnectionStatus> PollConnectionStatus();
//
// 设计选择：让策略实现自己的同步或异步细节（如 RealWifiStrategy 使用内部工作线程并
// 提供非阻塞的 Poll 接口），WifiManager 只负责作为轻量转发器并保持模板封装性。

template<typename WifiStrategy>
class WifiManager {
public:
    // 构造：将构造参数转发给策略的构造函数。对于需要显式路径的策略（例如
    // RealWifiStrategy），必须在构造时传入所需参数。
    template <typename... Args>
    explicit WifiManager(Args&&... args) : m_strategy(std::forward<Args>(args)...) {}

    ~WifiManager() = default;

    // 获取单例实例（必须在首次调用时提供与策略构造匹配的参数；后续调用无需重复）
    template <typename... Args>
    static WifiManager &GetInstance(Args&&... args) {
        static WifiManager instance(std::forward<Args>(args)...);
        return instance;
    }

    // 默认析构由类内部实现（用于停止 manager 线程）

    // --- 异步/同步请求接口 (由 LVGL 线程调用) ---
    // 直接转发给策略，由策略决定是否异步执行以及如何通过 Poll 接口回传结果。
    void RequestScan() { m_strategy.RequestScan(); }

    void RequestConnect(const std::string &ssid, const std::string &psk) { m_strategy.RequestConnect(ssid, psk); }

    void RequestSwitchToAppNetwork() { m_strategy.RequestSwitchNetwork(true); }

    void RequestSwitchToDevNetwork() { m_strategy.RequestSwitchNetwork(false); }

    // --- 结果轮询接口 ---
    // 由 LVGL 线程调用，或者由 Future/await 式异步 API 内部调用。
    std::optional<WifiScanResult> PollScanResult() { return m_strategy.PollScanResult(); }
    std::optional<ConnectionStatus> PollConnectionStatus() { return m_strategy.PollConnectionStatus(); }


    // --- Future/await 式异步 API ---
    // 为每个请求创建一个短生命周期线程，该线程轮询策略的 Poll* 直到得到结果或超时，
    // 然后满足对应的 promise 并退出。
    std::future<WifiScanResult> RequestScanAsync(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        std::promise<WifiScanResult> prom;
        auto fut = prom.get_future();

        // 触发策略请求（立即返回）
        m_strategy.RequestScan();

        // 启动单独线程为该请求轮询结果并满足 promise
        std::thread([this, prom = std::move(prom), timeout]() mutable {
            const std::chrono::milliseconds poll_interval(100);
            auto start = std::chrono::steady_clock::now();
            while (true) {
                // 在此处轮询结果
                auto opt = m_strategy.PollScanResult();
                if (opt) {
                    prom.set_value(std::move(*opt));
                    return;
                }
                if (timeout != std::chrono::milliseconds::max()) {
                    if (std::chrono::steady_clock::now() - start >= timeout) {
                        WifiScanResult r;
                        r.operationSuccess = false;
                        r.errorMessage = "timeout";
                        prom.set_value(std::move(r));
                        return;
                    }
                }
                std::this_thread::sleep_for(poll_interval);
            }
        }).detach();

        return fut;
    }

    std::future<ConnectionStatus>
    RequestConnectAsync(const std::string &ssid, const std::string &psk,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        std::promise<ConnectionStatus> prom;
        auto fut = prom.get_future();

        // 触发策略请求（立即返回）
        m_strategy.RequestConnect(ssid, psk);

        std::thread([this, prom = std::move(prom), timeout]() mutable {
            const std::chrono::milliseconds poll_interval(100);
            auto start = std::chrono::steady_clock::now();
            while (true) {
                // 在此处轮询结果
                auto opt = m_strategy.PollConnectionStatus();
                if (opt) {
                    prom.set_value(std::move(*opt));
                    return;
                }
                if (timeout != std::chrono::milliseconds::max()) {
                    if (std::chrono::steady_clock::now() - start >= timeout) {
                        ConnectionStatus s;
                        s.isConnected = false;
                        s.errorMessage = "timeout";
                        prom.set_value(std::move(s));
                        return;
                    }
                }
                std::this_thread::sleep_for(poll_interval);
            }
        }).detach();

        return fut;
    }

    WifiManager(const WifiManager &) = delete;
    WifiManager &operator=(const WifiManager &) = delete;

    WifiStrategy m_strategy; // 策略实例 (Real 或 Simulator)
};
