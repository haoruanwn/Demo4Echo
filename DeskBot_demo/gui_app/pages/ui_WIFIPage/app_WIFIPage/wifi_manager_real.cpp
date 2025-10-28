#include "wifi_manager_real.hpp"
#include <iostream>     // 替换为你的日志宏
#include <cstring>
#include <unistd.h>
#include "wpa_ctrl.h"   // 假设 wpa_ctrl.h 是 C-compatible

// 简单的日志
#define WIFI_LOG(fmt, ...) printf("[WIFI_CPP_SVC] " fmt "\n", ##__VA_ARGS__)

RealWifiManager::RealWifiManager(const std::string& iface_name) {
    iface_path_ = "/var/run/wpa_supplicant/" + iface_name;
}

RealWifiManager::~RealWifiManager() {
    deinit();
}

bool RealWifiManager::init() {
    // 1. 打开命令连接 (主线程)
    ctrl_cmd_ = wpa_ctrl_open(iface_path_.c_str());
    if (!ctrl_cmd_) {
        WIFI_LOG("Failed to open wpa_ctrl (cmd): %s", iface_path_.c_str());
        return false;
    }

    // 2. 打开监视器连接 (用于新线程)
    ctrl_monitor_ = wpa_ctrl_open(iface_path_.c_str());
    if (!ctrl_monitor_) {
        WIFI_LOG("Failed to open wpa_ctrl (monitor): %s", iface_path_.c_str());
        wpa_ctrl_close(ctrl_cmd_);
        ctrl_cmd_ = nullptr;
        return false;
    }

    // 3. 启动监视器线程
    monitorRunning_ = true;
    monitorThread_ = std::thread(&RealWifiManager::monitorLoop, this);

    WIFI_LOG("Wi-Fi Service Initialized (C++ Real).");
    return true;
}

void RealWifiManager::deinit() {
    if (monitorRunning_) {
        monitorRunning_ = false;
        
        // (注意: 鲁棒的实现需要一种方法来唤醒阻塞的 wpa_ctrl_recv)
        // (例如，在Linux上使用 eventfd 或 pipe)
        // (一个简单的(但不完美)的 hack 是关闭套接字)
        if(ctrl_monitor_) {
            wpa_ctrl_close(ctrl_monitor_); // 这将导致 monitorLoop 中的 recv 失败
            ctrl_monitor_ = nullptr;
        }

        if (monitorThread_.joinable()) {
            monitorThread_.join();
            WIFI_LOG("Monitor thread joined.");
        }
    }

    if (ctrl_cmd_) {
        wpa_ctrl_close(ctrl_cmd_);
        ctrl_cmd_ = nullptr;
    }
    
    // 如果 monitor 线程没有关闭它
    if (ctrl_monitor_) {
        wpa_ctrl_close(ctrl_monitor_);
        ctrl_monitor_ = nullptr;
    }
    
    WIFI_LOG("Wi-Fi Service Deinitialized.");
}

// [主线程]
int RealWifiManager::sendCmd(const char *cmd, char *reply_buf, size_t reply_buf_len) {
    if (!ctrl_cmd_) return -1;
    size_t len = reply_buf_len - 1;
    int ret = wpa_ctrl_request(ctrl_cmd_, cmd, strlen(cmd), reply_buf, &len, NULL);
    if (ret == 0) {
        reply_buf[len] = '\0';
    } else {
        strcpy(reply_buf, "ERROR");
    }
    return ret;
}

// [监控线程]
void RealWifiManager::monitorLoop() {
    char buf[2048];
    size_t len;

    if (wpa_ctrl_attach(ctrl_monitor_) != 0) {
        WIFI_LOG("Failed to attach to wpa_monitor");
        wpa_ctrl_close(ctrl_monitor_);
        ctrl_monitor_ = nullptr;
        monitorRunning_ = false; // 停止
        return;
    }
    WIFI_LOG("Monitor thread attached.");

    while (monitorRunning_) {
        if (!ctrl_monitor_) break; // 检查 deinit 是否已关闭它

        if (wpa_ctrl_pending(ctrl_monitor_) > 0) {
            len = sizeof(buf) - 1;
            if (wpa_ctrl_recv(ctrl_monitor_, buf, &len) == 0) {
                buf[len] = '\0';
                
                // 解析事件并推入队列
                std::lock_guard<std::mutex> lock(eventMutex_);
                if (strstr(buf, WPA_EVENT_SCAN_RESULTS)) {
                    WIFI_LOG("Event: Scan finished");
                    eventQueue_.push(AppEvent::SCAN_FINISHED);
                } else if (strstr(buf, WPA_EVENT_CONNECTED)) {
                    WIFI_LOG("Event: Connected");
                    eventQueue_.push(AppEvent::CONNECTED);
                } else if (strstr(buf, WPA_EVENT_DISCONNECTED)) {
                    WIFI_LOG("Event: Disconnected");
                    eventQueue_.push(AppEvent::DISCONNECTED);
                } else if (strstr(buf, "WRONG_KEY") || strstr(buf, WPA_EVENT_AUTH_REJECT)) {
                    WIFI_LOG("Event: Connection Failed (Auth)");
                    eventQueue_.push(AppEvent::AUTH_FAILED);
                }
            }
        } else {
            // 使用 wpa_ctrl_pending 的超时机制，而不是 usleep
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 200000; // 200ms
            fd_set rdfs;
            FD_ZERO(&rdfs);
            FD_SET(wpa_ctrl_get_fd(ctrl_monitor_), &rdfs);
            select(wpa_ctrl_get_fd(ctrl_monitor_) + 1, &rdfs, NULL, NULL, &tv);
        }
    }
    
    if(ctrl_monitor_) {
        wpa_ctrl_detach(ctrl_monitor_);
    }
    WIFI_LOG("Monitor thread detached.");
}

// [主线程]
void RealWifiManager::poll() {
    std::queue<AppEvent> localQueue;

    // 1. 快速抓取事件队列, 释放锁
    {
        std::lock_guard<std::mutex> lock(eventMutex_);
        localQueue.swap(eventQueue_);
    }

    // 2. 处理事件 (仍在主线程)
    while (!localQueue.empty()) {
        AppEvent event = localQueue.front();
        localQueue.pop();

        switch (event) {
            case AppEvent::SCAN_FINISHED:
                WIFI_LOG("Polling: Processing SCAN_FINISHED");
                currentStatus_ = WifiConnectionStatus::SCAN_FINISHED;
                statusDetails_ = "";
                newStatusFlag_ = true;
                // 现在获取结果 (仍在主线程)
                fetchScanResults();
                newScanFlag_ = true;
                break;
                
            case AppEvent::CONNECTED:
                WIFI_LOG("Polling: Processing CONNECTED");
                currentStatus_ = WifiConnectionStatus::CONNECTED;
                statusDetails_ = "";
                newStatusFlag_ = true;
                // 触发 DHCP
                WIFI_LOG("Triggering DHCP client (udhcpc)...");
                system("udhcpc -i wlan0 -n -q -b &");
                break;

            case AppEvent::DISCONNECTED:
                WIFI_LOG("Polling: Processing DISCONNECTED");
                currentStatus_ = WifiConnectionStatus::DISCONNECTED;
                statusDetails_ = "";
                newStatusFlag_ = true;
                break;

            case AppEvent::AUTH_FAILED:
                WIFI_LOG("Polling: Processing AUTH_FAILED");
                currentStatus_ = WifiConnectionStatus::CONNECTION_FAILED;
                statusDetails_ = "Wrong Password";
                newStatusFlag_ = true;
                break;
        }
    }
}

// [主线程]
void RealWifiManager::fetchScanResults() {
    static char reply_buf[4096]; // 保持静态以避免栈溢出
    
    lastScanResults_.clear();
    
    if (sendCmd("SCAN_RESULTS", reply_buf, sizeof(reply_buf)) == 0) {
        char *line = strtok(reply_buf, "\n");
        if (line) line = strtok(NULL, "\n"); // 跳过表头

        while (line != NULL) {
            WifiScanResult res;
            char bssid[32], flags[128], ssid[64];
            int signal_level;
            
            // 解析格式: bssid / frequency / signal level / flags / ssid
            int ret = sscanf(line, "%17s\t%*d\t%d\t%127[^\t]\t%63[^\n]", 
                             bssid, &signal_level, flags, ssid);
            
            if (ret == 4) {
                res.bssid = bssid;
                res.signal_level = signal_level;
                res.flags = flags;
                res.ssid = ssid;
                lastScanResults_.push_back(res);
            }
            line = strtok(NULL, "\n");
        }
    }
}

// [主线程]
void RealWifiManager::requestScan() {
    char reply[32];
    sendCmd("SCAN", reply, sizeof(reply));
    
    // 立即在内部设置状态
    currentStatus_ = WifiConnectionStatus::SCANNING;
    statusDetails_ = "";
    newStatusFlag_ = true; // 通知UI "正在扫描"
}

// [主线程]
void RealWifiManager::connect(const std::string& ssid, const std::string& psk) {
    char cmd[256];
    char reply[64];
    
    // 立即在内部设置状态
    currentStatus_ = WifiConnectionStatus::CONNECTING;
    statusDetails_ = "";
    newStatusFlag_ = true; // 通知UI "正在连接"
    
    // 1. 添加网络
    sendCmd("ADD_NETWORK", reply, sizeof(reply));
    int net_id = atoi(reply);
    if (net_id < 0) {
        WIFI_LOG("Failed to add network");
        currentStatus_ = WifiConnectionStatus::CONNECTION_FAILED;
        statusDetails_ = "ADD_NETWORK Failed";
        newStatusFlag_ = true;
        return;
    }

    // 2. 设置 SSID
    snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", net_id, ssid.c_str());
    sendCmd(cmd, reply, sizeof(reply));

    // 3. 设置密码 (或开放网络)
    if (!psk.empty()) {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", net_id, psk.c_str());
        sendCmd(cmd, reply, sizeof(reply));
    } else {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", net_id);
        sendCmd(cmd, reply, sizeof(reply));
    }

    // 4. 启用并选择网络
    snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", net_id);
    sendCmd(cmd, reply, sizeof(reply));
    
    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", net_id);
    sendCmd(cmd, reply, sizeof(reply));
}

// [主线程]
void RealWifiManager::disconnect() {
    char reply[32];
    sendCmd("DISCONNECT", reply, sizeof(reply));
}

// [主线程] - 消耗标志
bool RealWifiManager::getNewStatus(WifiConnectionStatus& status, std::string& details) {
    if (newStatusFlag_.load()) {
        status = currentStatus_;
        details = statusDetails_;
        newStatusFlag_ = false; // 消耗标志
        return true;
    }
    return false;
}

// [主线程] - 消耗标志
bool RealWifiManager::getNewScanResults(std::vector<WifiScanResult>& results) {
    if (newScanFlag_.load()) {
        results = lastScanResults_; // 返回拷贝
        newScanFlag_ = false; // 消耗标志
        return true;
    }
    return false;
}