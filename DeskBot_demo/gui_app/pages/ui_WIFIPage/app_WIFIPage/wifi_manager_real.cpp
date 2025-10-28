// app/wifi_manager_real.cpp
#include "wifi_manager_real.hpp"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <sstream>
#include "wpa_ctrl.h"

// 简单的日志
#define WIFI_LOG(fmt, ...) printf("[WIFI_CPP_SVC] " fmt "\n", ##__VA_ARGS__)

// [新增] NetworkInfo 结构体
struct NetworkInfo {
    int network_id;
    std::string ssid;
    std::string bssid;
    std::string flags;
};

// [新增] LIST_NETWORKS 解析器
// "network id / ssid / bssid / flags\n0\tMySSID\tany\t[CURRENT]\n1\tOtherSSID\tany\t[DISABLED]\n"
static std::vector<NetworkInfo> parseListNetworks(char* reply_buf) {
    std::vector<NetworkInfo> networks;
    std::stringstream ss(reply_buf);
    std::string line;

    // 跳过第一行 (表头)
    std::getline(ss, line);

    while (std::getline(ss, line)) {
        std::stringstream line_ss(line);
        NetworkInfo net;
        std::string segment;
        
        // C++ 的 sscanf 等价物
        if (std::getline(line_ss, segment, '\t')) {
            net.network_id = std::stoi(segment);
        } else continue;
        
        if (std::getline(line_ss, segment, '\t')) {
            net.ssid = segment;
        } else continue;
        
        if (std::getline(line_ss, segment, '\t')) {
            net.bssid = segment;
        } else continue;
        
        if (std::getline(line_ss, segment, '\t')) {
            net.flags = segment;
        } // 'flags' 可能是空的
        
        networks.push_back(net);
    }
    return networks;
}

RealWifiManager::RealWifiManager(const std::string& iface_name) 
    : iface_name_(iface_name) // [修改] 存储 iface_name
{
    iface_path_ = "/var/run/wpa_supplicant/" + iface_name;
}

RealWifiManager::~RealWifiManager() {
    deinit();
}

bool RealWifiManager::init() {
    WIFI_LOG("Taking over wpa_supplicant for App Mode...");

    // 1. 杀死开发者模式的实例
    if (system("killall wpa_supplicant") != 0) {
        WIFI_LOG("killall wpa_supplicant failed. (Maybe not running?)");
    }
    // [警告] 依赖 sleep 是脆弱的, 但对于方案A是必须的
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

    // 2. 启动 App 模式的新实例 (使用 _app.conf)
    std::string app_cmd = "wpa_supplicant -B -i " + iface_name_ + " -c /etc/wpa_supplicant_app.conf";
    WIFI_LOG("Executing: %s", app_cmd.c_str());
    if (system(app_cmd.c_str()) != 0) {
        WIFI_LOG("Failed to start App-Mode wpa_supplicant!");
        return false;
    }
    
    // [警告] 必须等待套接字创建
    std::this_thread::sleep_for(std::chrono::milliseconds(1000)); 

    // 3. 打开命令连接 (主线程)
    ctrl_cmd_ = wpa_ctrl_open(iface_path_.c_str());
    if (!ctrl_cmd_) {
        WIFI_LOG("Failed to open wpa_ctrl (cmd): %s. Cleaning up.", iface_path_.c_str());
        system("killall wpa_supplicant"); // 清理我们刚启动的进程
        return false;
    }

    // 4. 打开监视器连接 (用于新线程)
    ctrl_monitor_ = wpa_ctrl_open(iface_path_.c_str());
    if (!ctrl_monitor_) {
        WIFI_LOG("Failed to open wpa_ctrl (monitor): %s. Cleaning up.", iface_path_.c_str());
        wpa_ctrl_close(ctrl_cmd_);
        system("killall wpa_supplicant"); // 清理我们刚启动的进程
        return false;
    }

    // 5. 启动监视器线程
    monitorRunning_ = true;
    monitorThread_ = std::thread(&RealWifiManager::monitorLoop, this);

    WIFI_LOG("Wi-Fi Service Initialized (C++ Real, App Mode).");
    return true;
}

void RealWifiManager::deinit() {
    if (monitorRunning_.exchange(false)) { // 确保只执行一次
        WIFI_LOG("Restoring wpa_supplicant to Dev Mode...");
        
        // 1. (原逻辑) 关闭 monitor 线程和 wpa_ctrl 句柄
        if(ctrl_monitor_) {
            wpa_ctrl_close(ctrl_monitor_); 
            ctrl_monitor_ = nullptr;
        }
        if (monitorThread_.joinable()) {
            monitorThread_.join();
            WIFI_LOG("Monitor thread joined.");
        }
        if (ctrl_cmd_) {
            wpa_ctrl_close(ctrl_cmd_);
            ctrl_cmd_ = nullptr;
        }

        // 2. 杀死 App 模式的实例
        WIFI_LOG("Killing App-Mode wpa_supplicant...");
        if (system("killall wpa_supplicant") != 0) {
            WIFI_LOG("killall wpa_supplicant (app) failed.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 3. 恢复开发者模式的实例
        std::string dev_cmd = "wpa_supplicant -B -i " + iface_name_ + " -c /etc/wpa_supplicant_dev.conf";
        WIFI_LOG("Executing: %s", dev_cmd.c_str());
        if (system(dev_cmd.c_str()) != 0) {
             WIFI_LOG("Failed to restore Dev-Mode wpa_supplicant!");
        }
        
        // (可选) 立即触发 Dev 模式的 DHCP
        // std::string dhcp_cmd = "udhcpc -i " + iface_name_ + " -b";
        // system(dhcp_cmd.c_str());

        WIFI_LOG("Wi-Fi Service Deinitialized, Dev mode restored.");
    }
}

// [主线程]
int RealWifiManager::sendCmd(const char *cmd, char *reply_buf, size_t reply_buf_len) {
    if (!ctrl_cmd_) return -1;
    
    // 如果不需要回复
    if (!reply_buf || reply_buf_len == 0) {
        return wpa_ctrl_request(ctrl_cmd_, cmd, strlen(cmd), nullptr, nullptr, NULL);
    }
    
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

    if (!ctrl_monitor_ || wpa_ctrl_attach(ctrl_monitor_) != 0) {
        WIFI_LOG("Failed to attach to wpa_monitor");
        if(ctrl_monitor_) wpa_ctrl_close(ctrl_monitor_);
        ctrl_monitor_ = nullptr;
        monitorRunning_ = false; // 停止
        return;
    }
    WIFI_LOG("Monitor thread attached.");

    while (monitorRunning_) {
        if (!ctrl_monitor_) break; // 检查 deinit 是否已关闭它

        // 使用 wpa_ctrl_pending 的超时机制, 而不是 usleep
        struct timeval tv;
        tv.tv_sec = 1; // 1 秒超时
        tv.tv_usec = 0;
        
        fd_set rdfs;
        FD_ZERO(&rdfs);
        int fd = wpa_ctrl_get_fd(ctrl_monitor_);
        if (fd < 0) {
            WIFI_LOG("Monitor FD error.");
            break;
        }
        FD_SET(fd, &rdfs);

        int sel_ret = select(fd + 1, &rdfs, NULL, NULL, &tv);
        
        if (sel_ret < 0) { // 错误 (例如 deinit 关闭了)
            WIFI_LOG("Monitor select() error.");
            break;
        }
        
        if (sel_ret == 0) { // 超时
            continue; // 重新检查 monitorRunning_
        }

        // 有数据
        if (wpa_ctrl_pending(ctrl_monitor_) > 0) {
            len = sizeof(buf) - 1;
            if (ctrl_monitor_ && wpa_ctrl_recv(ctrl_monitor_, buf, &len) == 0) {
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
                
                // [修改] 触发 DHCP 和 SAVE_CONFIG
                WIFI_LOG("Triggering DHCP client (udhcpc)...");
                system("udhcpc -i wlan0 -n -q -b &");
                WIFI_LOG("Saving configuration...");
                sendCmd("SAVE_CONFIG", nullptr, 0); // 无需回复
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
    static char reply_buf[8192]; // 增加缓冲区
    
    lastScanResults_.clear();
    
    if (sendCmd("SCAN_RESULTS", reply_buf, sizeof(reply_buf)) == 0) {
        char *line = strtok(reply_buf, "\n");
        if (line) line = strtok(NULL, "\n"); // 跳过表头

        while (line != NULL) {
            WifiScanResult res;
            char bssid[32], flags[128], ssid[64];
            int signal_level;
            
            int ret = sscanf(line, "%17s\t%*d\t%d\t%127[^\t]\t%63[^\n]", 
                             bssid, &signal_level, flags, ssid);
            
            if (ret == 4 && strlen(ssid) > 0) { // 过滤掉隐藏 SSID
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
    // [修改] 先断开连接, 确保 wpa_supplicant 处于空闲状态以进行完整扫描
    // 这对于在已连接时切换网络至关重要
    WIFI_LOG("Disconnecting before scan...");
    sendCmd("DISCONNECT", nullptr, 0);
    
    // (可选) 给 wpa_supplicant 一点时间处理断连
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));

    WIFI_LOG("Requesting scan...");
    char reply[32];
    sendCmd("SCAN", reply, sizeof(reply));
    
    currentStatus_ = WifiConnectionStatus::SCANNING;
    statusDetails_ = "";
    newStatusFlag_ = true;
}

// [主线程]
void RealWifiManager::connect(const std::string& ssid, const std::string& psk) {
    char cmd[256];
    char reply[4096]; // 需要足够大以容纳 LIST_NETWORKS
    
    // 立即在内部设置状态
    currentStatus_ = WifiConnectionStatus::CONNECTING;
    statusDetails_ = "";
    newStatusFlag_ = true;
    
    int net_id = -1;

    // 1. [修改] 检查网络是否已存在
    sendCmd("LIST_NETWORKS", reply, sizeof(reply));
    auto networks = parseListNetworks(reply);
    
    for (const auto& net : networks) {
        if (net.ssid == ssid) {
            net_id = net.network_id;
            WIFI_LOG("Network '%s' already exists (ID: %d). Updating PSK.", ssid.c_str(), net_id);
            break;
        }
    }

    // 2. [修改] 如果不存在, 则添加
    if (net_id < 0) {
        WIFI_LOG("Network '%s' not found. Adding new network.", ssid.c_str());
        sendCmd("ADD_NETWORK", reply, sizeof(reply));
        net_id = atoi(reply);
        if (net_id < 0) {
            WIFI_LOG("Failed to add network");
            currentStatus_ = WifiConnectionStatus::CONNECTION_FAILED;
            statusDetails_ = "ADD_NETWORK Failed";
            newStatusFlag_ = true;
            return;
        }
        
        // 设置 SSID
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", net_id, ssid.c_str());
        sendCmd(cmd, reply, sizeof(reply));
    }

    // 3. [修改] 设置密码 (或开放网络)
    if (!psk.empty()) {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", net_id, psk.c_str());
        sendCmd(cmd, reply, sizeof(reply));
    } else {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", net_id);
        sendCmd(cmd, reply, sizeof(reply));
    }

    // 4. [修改] 启用并选择网络
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