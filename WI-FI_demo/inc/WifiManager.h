#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>

extern "C" {
    #include "wpa_ctrl.h"
}

// 用于存储扫描结果
struct WifiScanResult {
    std::string bssid;
    int frequency;
    int signal_level; // 信号强度, dBm
    std::string flags;
    std::string ssid;
};

// 内部事件类型
enum class WifiEvent {
    ScanFinished,
    Connected,
    Disconnected,
    ConnectionFailed
};

// C++ 核心类
class WifiManager {
public:
    // 构造函数，传入wlan接口名 (如 "wlan0") 和控制接口路径
    WifiManager(std::string iface_name, std::string ctrl_path = "/var/run/wpa_supplicant");
    ~WifiManager();

    // 启动监视器线程
    bool Start();
    // 停止监视器线程
    void Stop();

    // --- 异步命令 (发送给 wpa_supplicant) ---
    // 请求扫描 (结果将通过事件队列返回)
    bool RequestScan();
    // 连接到网络
    bool Connect(const std::string& ssid, const std::string& psk);
    // 断开连接
    bool Disconnect();

    // --- LVGL 轮询 ---
    // 在 LVGL timer 中调用此函数来处理事件
    // handler 将在 LVGL 线程中被调用
    void PollEvents(std::function<void(WifiEvent event, void* data)> handler);

private:
    // --- 内部辅助函数 ---
    // 向 wpa_supplicant 发送命令并获取回复
    std::string SendCommand(const std::string& cmd);
    // 解析 "SCAN_RESULTS" 的输出
    std::vector<WifiScanResult> ParseScanResults(const std::string& scan_output);
    
    // --- 线程 ---
    // 监视器线程的循环函数
    void MonitorThreadLoop();
    // 向队列中推送事件
    void PushEvent(WifiEvent event, void* data = nullptr);

    // --- 成员变量 ---
    std::string m_iface_name;
    std::string m_ctrl_path;

    struct wpa_ctrl* m_ctrl_cmd;     // 用于发送命令的连接
    struct wpa_ctrl* m_ctrl_monitor; // 用于接收事件的连接

    bool m_running;
    std::thread m_monitor_thread;
    
    std::queue<std::pair<WifiEvent, void*>> m_event_queue;
    std::mutex m_queue_mutex;
};