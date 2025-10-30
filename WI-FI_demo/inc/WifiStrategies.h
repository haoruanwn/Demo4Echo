#pragma once
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <optional>
#include <functional>
#include <atomic>


struct WifiScanResult {
    std::vector<std::string> ssids;
    bool operationSuccess = true;
    std::string errorMessage;
};

struct ConnectionStatus {
    bool isConnected;
    std::string ssid;
    std::string errorMessage; // "密码错误", "无响应" 等
};

// 模拟器和真实这两种策略作为鸭子类型存在，提供同样的接口，供 WifiManager 使用。
//   void RequestScan();
//   void RequestConnect(const std::string&, const std::string&);
//   void RequestSwitchNetwork(bool);
//   std::optional<WifiScanResult> PollScanResult();
//   std::optional<ConnectionStatus> PollConnectionStatus();

// --- 策略 模拟器实现 ---
// 使模拟器与 RealWifiStrategy 保持相同的鸭子接口：Request* 非阻塞地入队并立即返回，
// 后台工作线程执行任务并把结果放入结果队列，Poll* 非阻塞地读取结果队列。
#include <mutex>
#include <queue>
#include <condition_variable>

struct SimulatorWifiStrategy {
public:
    SimulatorWifiStrategy()
        : m_stopWorker(false), m_worker(&SimulatorWifiStrategy::WorkerLoop, this) {}

    ~SimulatorWifiStrategy() {
        // 停止后台线程并 join
        m_stopWorker.store(true);
        {
            std::lock_guard<std::mutex> lk(m_taskMutex);
        }
        m_taskCv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    // 非阻塞：将扫描任务入队并立即返回
    void RequestScan() {
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

    // 非阻塞：将连接任务入队并立即返回
    void RequestConnect(const std::string& ssid, const std::string& psk) {
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

    void RequestSwitchNetwork(bool /*toAppNetwork*/) {
        // 模拟：立即返回，不做实质操作
    }

    // 非阻塞轮询：若有结果则返回并从队列弹出
    std::optional<WifiScanResult> PollScanResult() {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        if (m_scanResults.empty()) return std::nullopt;
        WifiScanResult r = std::move(m_scanResults.front());
        m_scanResults.pop();
        return r;
    }

    std::optional<ConnectionStatus> PollConnectionStatus() {
        std::lock_guard<std::mutex> lk(m_resultMutex);
        if (m_connResults.empty()) return std::nullopt;
        ConnectionStatus s = std::move(m_connResults.front());
        m_connResults.pop();
        return s;
    }

private:
    // 简单的任务队列与后台线程
    void WorkerLoop() {
        while (!m_stopWorker.load()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_taskMutex);
                m_taskCv.wait(lk, [this]() { return m_stopWorker.load() || !m_tasks.empty(); });
                if (m_stopWorker.load() && m_tasks.empty()) break;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            try {
                if (task) task();
            } catch (...) {
                // 忽略模拟任务中的异常
            }
        }
    }

    std::atomic<bool> m_stopWorker;
    std::thread m_worker;

    std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    std::queue<std::function<void()>> m_tasks;

    // 结果队列
    std::mutex m_resultMutex;
    std::queue<WifiScanResult> m_scanResults;
    std::queue<ConnectionStatus> m_connResults;
};

// --- 策略 真实实现 ---
// 使用 wpa_ctrl 与 wpa_supplicant 通信
#include "ThreadSafeQueue.h"
struct RealWifiStrategy {
public:
    // 构造函数：启动 m_workerThread
    RealWifiStrategy(); // User to implement

    // 析构函数：发送停止信号, m_workerThread.join()
    ~RealWifiStrategy(); // User to implement

    // --- 策略的公共接口 (由 WifiManager 调用) ---
    
    /**
     * @brief [LVGL 线程调用] 请求扫描
     */
    void RequestScan() {
        // 将一个“发送 SCAN 命令”的任务推入队列
        m_taskQueue.push([this]() { this->DoScanRequest(); });
    }

    /**
     * @brief [LVGL 线程调用] 请求连接
     */
    void RequestConnect(const std::string& ssid, const std::string& psk) {
        m_taskQueue.push([this, ssid, psk]() { 
            this->DoConnectRequest(ssid, psk); 
        });
    }

    /**
     * @brief [LVGL 线程调用] 切换网络
     */
    void RequestSwitchNetwork(bool toAppNetwork) {
        m_taskQueue.push([this, toAppNetwork]() { 
            this->DoSwitchNetwork(toAppNetwork); 
        });
    }

    /**
     * @brief [LVGL 线程调用] 轮询扫描结果 (非阻塞)
     */
    std::optional<WifiScanResult> PollScanResult() {
        return m_scanResultQueue.try_pop();
    }

    /**
     * @brief [LVGL 线程调用] 轮询连接状态 (非阻塞)
     */
    std::optional<ConnectionStatus> PollConnectionStatus() {
        return m_connectionStatusQueue.try_pop();
    }

private:
    // --- 内部状态 ---
    struct wpa_ctrl* m_ctrl_if = nullptr; // 命令接口
    struct wpa_ctrl* m_mon_if = nullptr;  // 事件监控接口
    std::thread m_workerThread;
    std::atomic<bool> m_stopWorker = false;

    // --- 队列 (与之前相同) ---
    ThreadSafeQueue<std::function<void()>> m_taskQueue;
    ThreadSafeQueue<WifiScanResult> m_scanResultQueue;
    ThreadSafeQueue<ConnectionStatus> m_connectionStatusQueue;

    // --- 核心工作循环 (在 m_workerThread 中运行) ---
    
    /**
     * @brief 工作线程的主循环
     * 这是整个设计的核心。
     */
    void WorkerLoop() {
        // 算法描述 (Algorithm Description):
        // 1. 调用 wpa_ctrl_open() 两次，分别初始化 m_ctrl_if 和 m_mon_if。
        // 2. 检查句柄是否有效。
        // 3. 调用 wpa_ctrl_attach(m_mon_if) 来开始监听事件。
        // 4. 获取任务队列的 eventfd (m_taskQueue.get_event_fd())。
        // 5. 获取 wpa_supplicant 的监控 FD (wpa_ctrl_get_fd(m_mon_if))。
        // 6.
        //    while (!m_stopWorker) {
        // 7.     设置 pollfd 结构体，监听 task_fd 和 mon_fd 的 POLLIN 事件。
        // 8.     调用 poll() (或 select())，带超时。
        // 9.
        // 10.    if (task_fd 可读) {
        // 11.        清空 eventfd。
        // 12.        while (m_taskQueue.try_pop(task)) {
        // 13.            task(); // 执行来自 LVGL 的任务 (例如 DoScanRequest)
        // 14.        }
        // 15.    }
        // 16.
        // 17.    if (mon_fd 可读) {
        // 18.        HandleWpaEvents(); // 处理来自 wpa_supplicant 的事件
        // 19.    }
        // 20. }
        // 21. 清理：wpa_ctrl_close(m_ctrl_if), wpa_ctrl_close(m_mon_if)。
        
        // User to implement
    }

    /**
     * @brief [工作线程调用] 处理来自 wpa_supplicant 的异步事件
     */
    void HandleWpaEvents() {
        // 算法描述:
        // 1. char buf[2048];
        // 2. while (wpa_ctrl_pending(m_mon_if) > 0) {
        // 3.     wpa_ctrl_recv(m_mon_if, buf, &len);
        // 4.     std::string event(buf, len);
        // 5.
        // 6.     // (这是最脆弱的部分：字符串解析)
        // 7.     if (event.contains("CTRL-EVENT-SCAN-RESULTS")) {
        // 8.         // 扫描完成了，现在我们去 *获取* 结果
        // 9.         DoGetScanResults();
        // 10.    } else if (event.contains("CTRL-EVENT-CONNECTED")) {
        // 11.        m_connectionStatusQueue.push(ConnectionStatus{...});
        // 12.    } else if (event.contains("CTRL-EVENT-DISCONNECTED ... reason=WRONG_KEY")) {
        // 13.        m_connectionStatusQueue.push(ConnectionStatus{... , "密码错误"});
        // 14.    } else if (event.contains("CTRL-EVENT-SSID-TEMP-DISABLED ... reason=WRONG_KEY")) {
        // 15.        m_connectionStatusQueue.push(ConnectionStatus{... , "密码错误"});
        // 16.    } // ... 还有很多其他事件
        // 17. }
        // User to implement
    }

    // --- 具体的 wpa_ctrl 命令封装 ---
    
    /**
     * @brief [工作线程调用] 向 wpa_supplicant 发送 "SCAN" 命令
     */
    void DoScanRequest() {
        // wpa_ctrl_request 是一个 *阻塞* 调用，它等待 "OK" 或 "FAIL"
        // 但它通常很快，因为它只确认命令已被接收。
        // 真正的扫描结果是稍后通过 m_mon_if 异步传回的。
        
        // 伪代码:
        // char reply[1024];
        // wpa_ctrl_request(m_ctrl_if, "SCAN", 4, reply, &len, ...);
        // User to implement
    }

    /**
     * @brief [工作线程调用] 在 "SCAN-RESULTS" 事件后获取扫描列表
     */
    void DoGetScanResults() {
        // 伪代码:
        // char scan_results[...];
        // wpa_ctrl_request(m_ctrl_if, "SCAN_RESULTS", 12, scan_results, ...);
        // 
        // WifiScanResult result;
        // (手动解析 'scan_results' 字符串，它是一个多行文本)
        // (例如: bssid / frequency / signal level / flags / ssid)
        // (for 循环解析每一行，提取 ssid)
        // result.ssids.push_back(parsed_ssid);
        //
        // m_scanResultQueue.push(result);
        // User to implement
    }

    /**
     * @brief [工作线程调用] 向 wpa_supplicant 发送连接命令
     */
    void DoConnectRequest(const std::string& ssid, const std::string& psk) {
        // 伪代码:
        // 1. wpa_ctrl_request(m_ctrl_if, "ADD_NETWORK", ...) -> "0" (network_id)
        // 2. wpa_ctrl_request(m_ctrl_if, "SET_NETWORK 0 ssid \"...\"", ...)
        // 3. wpa_ctrl_request(m_ctrl_if, "SET_NETWORK 0 psk \"...\"", ...)
        // 4. wpa_ctrl_request(m_ctrl_if, "ENABLE_NETWORK 0", ...)
        // 5. wpa_ctrl_request(m_ctrl_if, "SELECT_NETWORK 0", ...)
        //
        // (连接结果将通过 m_mon_if 异步返回 "CTRL-EVENT-CONNECTED" 或 "DISCONNECTED")
        // User to implement
    }

    void DoSwitchNetwork(bool toAppNetwork) {
        // 伪代码:
        // if (toAppNetwork) {
        //     wpa_ctrl_request(m_ctrl_if, "SELECT_NETWORK app_network_id", ...);
        // } else {
        //     wpa_ctrl_request(m_ctrl_if, "SELECT_NETWORK wifi_network_id", ...);
        // }
        // User to implement
    }
};