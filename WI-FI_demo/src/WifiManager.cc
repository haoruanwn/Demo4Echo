#include "./inc/WifiManager.h"
#include <unistd.h> // for sleep
#include <cstring> // for strstr, strcmp, etc.

WifiManager::WifiManager(std::string iface_name, std::string ctrl_path)
    : m_iface_name(iface_name), 
      m_ctrl_path(ctrl_path + "/" + iface_name), // 完整的套接字路径
      m_ctrl_cmd(nullptr), 
      m_ctrl_monitor(nullptr), 
      m_running(false) {}

WifiManager::~WifiManager() {
    Stop();
}

bool WifiManager::Start() {
    // 1. 打开命令连接
    m_ctrl_cmd = wpa_ctrl_open(m_ctrl_path.c_str());
    if (!m_ctrl_cmd) {
        perror("WifiManager: wpa_ctrl_open (cmd) failed");
        return false;
    }

    // 2. 打开监视器连接
    m_ctrl_monitor = wpa_ctrl_open(m_ctrl_path.c_str());
    if (!m_ctrl_monitor) {
        perror("WifiManager: wpa_ctrl_open (monitor) failed");
        wpa_ctrl_close(m_ctrl_cmd);
        return false;
    }

    // 3. 附加到事件监视器
    if (wpa_ctrl_attach(m_ctrl_monitor) != 0) {
        perror("WifiManager: wpa_ctrl_attach failed");
        wpa_ctrl_close(m_ctrl_cmd);
        wpa_ctrl_close(m_ctrl_monitor);
        return false;
    }

    // 4. 启动监视器线程
    m_running = true;
    m_monitor_thread = std::thread(&WifiManager::MonitorThreadLoop, this);
    
    return true;
}

void WifiManager::Stop() {
    m_running = false;
    
    // TODO: 需要一种方法来唤醒阻塞的 wpa_ctrl_recv
    // (例如，向套接字发送一个虚拟字节或关闭它)
    
    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }

    if (m_ctrl_monitor) {
        wpa_ctrl_detach(m_ctrl_monitor);
        wpa_ctrl_close(m_ctrl_monitor);
        m_ctrl_monitor = nullptr;
    }
    if (m_ctrl_cmd) {
        wpa_ctrl_close(m_ctrl_cmd);
        m_ctrl_cmd = nullptr;
    }
}

// 监视器线程的主循环
void WifiManager::MonitorThreadLoop() {
    char buf[4096];
    size_t len;

    while (m_running) {
        // 检查是否有待处理的事件
        if (wpa_ctrl_pending(m_ctrl_monitor) > 0) {
            len = sizeof(buf) - 1;
            if (wpa_ctrl_recv(m_ctrl_monitor, buf, &len) == 0) {
                buf[len] = '\0';
                
                // --- 事件解析 ---
                // (这是简化的，实际的wpa_supplicant事件很复杂)
                if (strstr(buf, WPA_EVENT_SCAN_RESULTS)) {
                    // 扫描完成事件
                    // LVGL 模拟代码是在 scan_finished_cb 中获取结果的
                    // 这里我们采用 "SCAN" -> "SCAN_RESULTS" -> "SCAN_RESULTS" (command) 的流程
                    
                    // 1. 收到"扫描完成"事件，我们立即发送"SCAN_RESULTS"命令来获取数据
                    std::string scan_data = SendCommand("SCAN_RESULTS");
                    
                    // 2. 解析数据
                    auto* results = new std::vector<WifiScanResult>(ParseScanResults(scan_data));
                    
                    // 3. 推送事件
                    PushEvent(WifiEvent::ScanFinished, results);

                } else if (strstr(buf, WPA_EVENT_CONNECTED)) {
                    // 连接成功
                    PushEvent(WifiEvent::Connected);
                    
                    // *** 关键步骤：触发 DHCP ***
                    // wpa_supplicant 不负责获取 IP，它连接成功后，
                    // 我们必须像你手动操作那样，运行 DHCP 客户端。
                    // 这是少数几个适合用 fork/exec 的地方。
                    if (fork() == 0) {
                        // 子进程
                        execl("/sbin/udhcpc", "udhcpc", "-i", m_iface_name.c_str(), "-n", "-q", "-b", (char*)NULL);
                        // -n: 失败后退出, -q: 安静, -b: 后台
                        exit(1); // exec 失败
                    }

                } else if (strstr(buf, WPA_EVENT_DISCONNECTED)) {
                    PushEvent(WifiEvent::Disconnected);
                }
                // ... 添加更多事件处理，例如 WPA_EVENT_AUTH_REJECT (密码错误)
            }
        } else {
            // 避免忙循环
            usleep(100000); // 100ms
        }
    }
}

// 在 LVGL 线程中调用
void WifiManager::PollEvents(std::function<void(WifiEvent, void*)> handler) {
    std::unique_lock<std::mutex> lock(m_queue_mutex);
    while (!m_event_queue.empty()) {
        auto event_pair = m_event_queue.front();
        m_event_queue.pop();
        
        // 解锁，以便 handler 可以自由调用 C++ 库的其他函数 (尽管不推荐)
        lock.unlock(); 
        
        // 调用 LVGL 传入的处理器
        handler(event_pair.first, event_pair.second);
        
        lock.lock(); // 重新锁定以进行下一次 while 循环检查
    }
}

void WifiManager::PushEvent(WifiEvent event, void* data) {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_event_queue.push({event, data});
}

// 发送命令的封装
std::string WifiManager::SendCommand(const std::string& cmd) {
    char reply[4096]; // 增加缓冲区
    size_t reply_len = sizeof(reply) - 1;
    
    if (wpa_ctrl_request(m_ctrl_cmd, cmd.c_str(), cmd.length(), reply, &reply_len, NULL) == 0) {
        reply[reply_len] = '\0';
        return std::string(reply);
    }
    return "ERROR";
}

// 异步请求扫描
bool WifiManager::RequestScan() {
    std::string reply = SendCommand("SCAN");
    return (reply.find("OK") != std::string::npos);
}

// 异步请求连接
bool WifiManager::Connect(const std::string& ssid, const std::string& psk) {
    // 1. 添加一个新网络
    std::string reply = SendCommand("ADD_NETWORK");
    if (reply.find("ERROR") != std::string::npos) return false;
    
    std::string net_id = reply;
    net_id.pop_back(); // 移除换行符
    
    // 2. 设置 SSID (注意引号)
    SendCommand("SET_NETWORK " + net_id + " ssid \"" + ssid + "\"");
    
    // 3. 设置密码
    if (psk.empty()) {
        // 开放网络
        SendCommand("SET_NETWORK " + net_id + " key_mgmt NONE");
    } else {
        // WPA-PSK
        SendCommand("SET_NETWORK " + net_id + " psk \"" + psk + "\"");
        SendCommand("SET_NETWORK " + net_id + " key_mgmt WPA-PSK"); // 确保
    }
    
    // 4. 启用并选择网络
    SendCommand("ENABLE_NETWORK " + net_id);
    SendCommand("SELECT_NETWORK " + net_id);
    
    // 5. (可选) 保存配置，使其持久化
    // SendCommand("SAVE_CONFIG"); 
    // 注意：这会修改 /etc/wpa_supplicant.conf，根据你的需求决定是否调用

    return true;
}

// (ParseScanResults 的实现比较繁琐，需要逐行解析 "SCAN_RESULTS" 的输出)
std::vector<WifiScanResult> WifiManager::ParseScanResults(const std::string& scan_output) {
    // ... 
    // 解析 "bssid / frequency / signal level / flags / ssid" 格式的
    // ...
    return std::vector<WifiScanResult>(); // 返回解析后的列表
}