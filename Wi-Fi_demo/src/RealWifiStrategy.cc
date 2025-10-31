#include "../inc/RealWifiStrategy.h"
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <vector>
#include <wpa_ctrl.h>

// 构造函数必须提供路径与接口名称
RealWifiStrategy::RealWifiStrategy(const std::string &ctrlPath, const std::string &ifaceName,
                                   const std::string &wpaConfApp, const std::string &wpaConfDev,
                                   bool auto_dhcp, const std::string &dhcpClientCmd) :
    m_ctrl_path(ctrlPath), m_iface_name(ifaceName), m_wpa_conf_app(wpaConfApp), m_wpa_conf_dev(wpaConfDev),
    m_auto_dhcp(auto_dhcp), m_dhcp_cmd(dhcpClientCmd) {
    // 启动工作线程
    m_workerThread = std::thread([this]() { this->WorkerLoop(); });
}

RealWifiStrategy::~RealWifiStrategy() {
    // signal worker to stop and wake it by pushing an empty task
    m_stopWorker.store(true);
    try {
        m_taskQueue.push(std::function<void()>());
    } catch (...) {
        // ignore push failures during shutdown
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    // 清理 wpa_ctrl 接口
    if (m_mon_if) {
        wpa_ctrl_detach(m_mon_if);
        wpa_ctrl_close(m_mon_if);
        m_mon_if = nullptr;
    }

    if (m_ctrl_if) {
        wpa_ctrl_close(m_ctrl_if);
        m_ctrl_if = nullptr;
    }
}

void RealWifiStrategy::RequestScan() {
    m_taskQueue.push([this]() { this->DoScanRequest(); });
}

void RealWifiStrategy::RequestConnect(const std::string &ssid, const std::string &psk) {
    m_taskQueue.push([this, ssid, psk]() { this->DoConnectRequest(ssid, psk); });
}

void RealWifiStrategy::RequestSwitchNetwork(bool toAppNetwork) {
    m_taskQueue.push([this, toAppNetwork]() { this->DoSwitchNetwork(toAppNetwork); });
}

std::optional<WifiScanResult> RealWifiStrategy::PollScanResult() { return m_scanResultQueue.try_pop(); }

std::optional<ConnectionStatus> RealWifiStrategy::PollConnectionStatus() { return m_connectionStatusQueue.try_pop(); }

// 工作线程主循环，接受任务并处理 WPA 事件
void RealWifiStrategy::WorkerLoop() {
    // 通过构造函数注入的路径和接口名称打开控制接口
    std::string ctrl_socket;
    if (!m_ctrl_path.empty()) {
        ctrl_socket = m_ctrl_path;
        if (!ctrl_socket.empty() && ctrl_socket.back() != '/')
            ctrl_socket.push_back('/');
        // 把它转换为大概这么个形式：/var/run/wpa_supplicant/，再在后面加上接口名
        ctrl_socket += m_iface_name;
        // e.g. /var/run/wpa_supplicant/wlan0
    }

    // 打开控制接口，并传给m_ctrl_if
    m_ctrl_if = nullptr;
    if (!ctrl_socket.empty()) {
        m_ctrl_if = wpa_ctrl_open(ctrl_socket.c_str());
    }
    if (!m_ctrl_if) {
        // fallback to using iface name directly (some wpa_ctrl implementations accept this)
        m_ctrl_if = wpa_ctrl_open(m_iface_name.c_str());
    }

    // Try to open a monitor interface for events
    m_mon_if = nullptr;
    if (m_ctrl_if) {
        if (!ctrl_socket.empty()) {
            m_mon_if = wpa_ctrl_open(ctrl_socket.c_str());
        }
        if (!m_mon_if) {
            m_mon_if = wpa_ctrl_open(m_iface_name.c_str());
        }
        if (m_mon_if) {
            // Attach to receive events where supported. Ignore return value.
            wpa_ctrl_attach(m_mon_if);
        }
    }

    // 启动从任务队列处理任务的循环，如果队列关闭则退出
    while (!m_stopWorker.load()) {
        // 从任务队列中弹出任务
        auto opt = m_taskQueue.pop();
        if (!opt)
            break; // queue closed
        auto task = std::move(*opt);
        if (task) {
            // run the task which will call Do* methods
            task();
        }

        // If monitor interface exists, try to read any pending events (non-blocking).
        if (m_mon_if) {
            // Use wpa_ctrl_pending + wpa_ctrl_recv if available; fall back to
            // a non-blocking recv via poll on the control socket fd.
            // wpa_ctrl_get_fd is used to obtain fd for poll.
            int fd = -1;
            fd = wpa_ctrl_get_fd(m_mon_if);
            if (fd >= 0) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLIN;
                int rv = poll(&pfd, 1, 0);
                if (rv > 0 && (pfd.revents & POLLIN)) {
                    HandleWpaEvents();
                }
            }
        }
    }

    // while循环结束了，清理控制接口
    // Clean up control interfaces
    if (m_mon_if) {
        wpa_ctrl_detach(m_mon_if);
        wpa_ctrl_close(m_mon_if);
        m_mon_if = nullptr;
    }
    if (m_ctrl_if) {
        wpa_ctrl_close(m_ctrl_if);
        m_ctrl_if = nullptr;
    }
}

void RealWifiStrategy::HandleWpaEvents() {
    if (!m_mon_if)
        return;
    // Try to receive event into buffer
    char buf[4096];
    size_t len = sizeof(buf) - 1;
    if (wpa_ctrl_pending(m_mon_if)) {
        if (wpa_ctrl_recv(m_mon_if, buf, &len) == 0) {
            buf[len] = '\0';
            std::string ev(buf, len);
            // Handle some common events
            if (ev.find("CTRL-EVENT-SCAN-RESULTS") != std::string::npos) {
                // New scan results available
                DoGetScanResults();
            } else if (ev.find("CTRL-EVENT-CONNECTED") != std::string::npos) {
                ConnectionStatus s;
                s.isConnected = true;
                // try to extract ssid from event
                auto pos = ev.find("ssid=");
                if (pos != std::string::npos) {
                    s.ssid = ev.substr(pos + 5);
                }
                m_connectionStatusQueue.push(std::move(s));
            } else if (ev.find("CTRL-EVENT-DISCONNECTED") != std::string::npos) {
                ConnectionStatus s;
                s.isConnected = false;
                s.errorMessage = "disconnected";
                m_connectionStatusQueue.push(std::move(s));
            }
        }
    }
}

void RealWifiStrategy::DoScanRequest() {
    if (!m_ctrl_if) {
        WifiScanResult r;
        r.operationSuccess = false;
        r.errorMessage = "no ctrl interface";
        m_scanResultQueue.push(std::move(r));
        return;
    }

    char buf[128];
    size_t len = sizeof(buf) - 1;
    // send SCAN command
    int res = wpa_ctrl_request(m_ctrl_if, "SCAN", 4, buf, &len, nullptr);
    if (res != 0) {
        WifiScanResult r;
        r.operationSuccess = false;
        r.errorMessage = "SCAN command failed";
        m_scanResultQueue.push(std::move(r));
        return;
    }

    // After requesting scan, try to fetch results (some wpa_supplicant signal via events).
    // Sleep a short while to allow scan to complete; DoGetScanResults will also be
    // invoked if monitor signals scan results.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    DoGetScanResults();
}

void RealWifiStrategy::DoGetScanResults() {
    if (!m_ctrl_if) {
        WifiScanResult r;
        r.operationSuccess = false;
        r.errorMessage = "no ctrl interface";
        m_scanResultQueue.push(std::move(r));
        return;
    }

    // Request SCAN_RESULTS
    std::vector<char> buf(8192);
    size_t len = buf.size() - 1;
    int res = wpa_ctrl_request(m_ctrl_if, "SCAN_RESULTS", 12, buf.data(), &len, nullptr);
    if (res != 0) {
        WifiScanResult r;
        r.operationSuccess = false;
        r.errorMessage = "SCAN_RESULTS request failed";
        m_scanResultQueue.push(std::move(r));
        return;
    }
    buf[len] = '\0';
    std::string data(buf.data(), len);

    WifiScanResult result;
    result.operationSuccess = true;

    // SCAN_RESULTS format: first line is header, following lines: bssid\tfrequency\tsignal level\tflags\tssid
    std::istringstream iss(data);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (first) {
            first = false;
            continue;
        }
        if (line.empty())
            continue;
        // split by tabs; ssid is last field
        std::string ssid;
        size_t pos = line.rfind('\t');
        if (pos != std::string::npos) {
            ssid = line.substr(pos + 1);
        } else {
            // fallback: split by spaces and take last token
            std::istringstream ls(line);
            std::string token;
            std::vector<std::string> toks;
            while (ls >> token)
                toks.push_back(token);
            if (!toks.empty())
                ssid = toks.back();
        }
        if (!ssid.empty())
            result.ssids.push_back(ssid);
    }

    m_scanResultQueue.push(std::move(result));
}

void RealWifiStrategy::DoConnectRequest(const std::string &ssid, const std::string &psk) {
    if (!m_ctrl_if) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "no ctrl interface";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    // 1) add network
    char buf[128];
    size_t len = sizeof(buf) - 1;
    int r = wpa_ctrl_request(m_ctrl_if, "ADD_NETWORK", 11, buf, &len, nullptr);
    if (r != 0) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "ADD_NETWORK failed";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }
    buf[len] = '\0';
    std::string id_str(buf, len);
    // trim
    id_str.erase(id_str.find_last_not_of(" \n\r\t") + 1);

    // Prepare and set SSID/PSK
    std::string cmd;
    len = sizeof(buf) - 1;
    cmd = "SET_NETWORK " + id_str + " ssid \"" + ssid + "\"";
    r = wpa_ctrl_request(m_ctrl_if, cmd.c_str(), cmd.size(), buf, &len, nullptr);
    if (r != 0) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "SET_NETWORK ssid failed";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    // set psk or open network
    if (!psk.empty()) {
        len = sizeof(buf) - 1;
        cmd = "SET_NETWORK " + id_str + " psk \"" + psk + "\"";
        r = wpa_ctrl_request(m_ctrl_if, cmd.c_str(), cmd.size(), buf, &len, nullptr);
    } else {
        len = sizeof(buf) - 1;
        cmd = "SET_NETWORK " + id_str + " key_mgmt NONE";
        r = wpa_ctrl_request(m_ctrl_if, cmd.c_str(), cmd.size(), buf, &len, nullptr);
    }
    if (r != 0) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "SET_NETWORK psk failed";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    // enable and select
    len = sizeof(buf) - 1;
    cmd = "ENABLE_NETWORK " + id_str;
    r = wpa_ctrl_request(m_ctrl_if, cmd.c_str(), cmd.size(), buf, &len, nullptr);
    if (r != 0) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "ENABLE_NETWORK failed";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    len = sizeof(buf) - 1;
    cmd = "SELECT_NETWORK " + id_str;
    r = wpa_ctrl_request(m_ctrl_if, cmd.c_str(), cmd.size(), buf, &len, nullptr);
    if (r != 0) {
        ConnectionStatus s;
        s.isConnected = false;
        s.errorMessage = "SELECT_NETWORK failed";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    // Poll STATUS until wpa_state=COMPLETED or timeout
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::vector<char> statusBuf(1024);
        size_t slen = statusBuf.size() - 1;
        int rr = wpa_ctrl_request(m_ctrl_if, "STATUS", 6, statusBuf.data(), &slen, nullptr);
        if (rr != 0)
            continue;
        statusBuf[slen] = '\0';
        std::string st(statusBuf.data(), slen);
        if (st.find("wpa_state=COMPLETED") != std::string::npos) {
            ConnectionStatus s;
            s.isConnected = true;
            s.ssid = ssid;
            m_connectionStatusQueue.push(std::move(s));

            // After association is complete, ensure the interface has an IPv4
            // address. If not and auto DHCP is enabled, attempt to start the
            // configured DHCP client (e.g. udhcpc). Use getifaddrs to check
            // for existing AF_INET address.
            if (m_auto_dhcp) {
                bool has_ipv4 = false;
                struct ifaddrs *ifaddr = nullptr;
                if (getifaddrs(&ifaddr) == 0) {
                    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
                        if (!ifa->ifa_addr) continue;
                        if (ifa->ifa_addr->sa_family == AF_INET && ifa->ifa_name && m_iface_name == ifa->ifa_name) {
                            has_ipv4 = true;
                            break;
                        }
                    }
                    freeifaddrs(ifaddr);
                }

                if (!has_ipv4) {
                    // check if dhcp client exists in PATH
                    std::string check_cmd = std::string("command -v ") + m_dhcp_cmd + " >/dev/null 2>&1";
                    int rc = system(check_cmd.c_str());
                    if (rc == 0) {
                        // avoid starting duplicate clients for the same iface
                        std::string pgrep_cmd = std::string("pgrep -f \"") + m_dhcp_cmd + " .* -i " + m_iface_name + " >/dev/null 2>&1";
                        int already = system(pgrep_cmd.c_str());
                        if (already != 0) {
                            std::string dhcp_cmd = m_dhcp_cmd + " -i " + m_iface_name + " >/dev/null 2>&1 &";
                            system(dhcp_cmd.c_str());
                            ConnectionStatus d;
                            d.isConnected = true;
                            d.ssid = ssid;
                            d.errorMessage = std::string("launched DHCP client: ") + m_dhcp_cmd;
                            m_connectionStatusQueue.push(std::move(d));
                        }
                    } else {
                        ConnectionStatus info;
                        info.isConnected = true;
                        info.ssid = ssid;
                        info.errorMessage = std::string("DHCP client not found: ") + m_dhcp_cmd;
                        m_connectionStatusQueue.push(std::move(info));
                    }
                }
            }

            return;
        }
    }

    ConnectionStatus s;
    s.isConnected = false;
    s.errorMessage = "connection timeout or failed";
    m_connectionStatusQueue.push(std::move(s));
}

void RealWifiStrategy::DoSwitchNetwork(bool toAppNetwork) {
    // 根据bool变量来选择是切换到app还是dev网络
    // true表示切换到app网络，false表示切换到dev网络

    // 链接状态初始化为未连接
    ConnectionStatus s;
    s.isConnected = false;

    // 通过构造函数注入的参数来指定控制socket路径，例如/var/run/wpa_supplicant/wlan0
    std::string ctrl_dir = m_ctrl_path; 
    std::string ctrl_socket;
    if (!ctrl_dir.empty()) {
        ctrl_socket = ctrl_dir;
        if (!ctrl_socket.empty() && ctrl_socket.back() != '/')
            ctrl_socket.push_back('/');
        ctrl_socket += m_iface_name;
    }

    // 选择配置文件
    const std::string &conf = toAppNetwork ? m_wpa_conf_app : m_wpa_conf_dev;
    s.errorMessage = std::string("switching to ") + (toAppNetwork ? "app" : "dev");
    m_connectionStatusQueue.push(s);

    // Try to reuse an existing wpa_supplicant control socket if possible. This
    // avoids killing a system-managed wpa_supplicant or stepping on another
    // process that already controls the interface.
    // 尝试复用现有的控制socket
    struct wpa_ctrl *try_ctrl = nullptr;
    if (!ctrl_socket.empty())
        try_ctrl = wpa_ctrl_open(ctrl_socket.c_str());
    if (!try_ctrl)
        try_ctrl = wpa_ctrl_open(m_iface_name.c_str());
    if (try_ctrl) {
        // Reuse existing control interface: replace our handles and attach monitor
        if (m_mon_if) {
            wpa_ctrl_detach(m_mon_if);
            wpa_ctrl_close(m_mon_if);
            m_mon_if = nullptr;
        }
        if (m_ctrl_if) {
            wpa_ctrl_close(m_ctrl_if);
            m_ctrl_if = nullptr;
        }

        m_ctrl_if = try_ctrl;
        if (!ctrl_socket.empty())
            m_mon_if = wpa_ctrl_open(ctrl_socket.c_str());
        if (!m_mon_if)
            m_mon_if = wpa_ctrl_open(m_iface_name.c_str());
        if (m_mon_if)
            wpa_ctrl_attach(m_mon_if);

        ConnectionStatus reused;
        reused.isConnected = false;
        reused.errorMessage = "reused existing wpa_supplicant control socket";
        m_connectionStatusQueue.push(std::move(reused));
        return;
    }

    // If a ctrl path was provided and the socket file exists but couldn't be
    // opened, it may be stale (leftover from unclean termination). Remove it
    // to allow a fresh wpa_supplicant to create a new socket. If no ctrl path
    // was provided, skip filesystem operations and rely on iface-name fallback.
    if (!ctrl_socket.empty()) {
        struct stat st;
        if (stat(ctrl_socket.c_str(), &st) == 0) {
            if (S_ISSOCK(st.st_mode)) {
                unlink(ctrl_socket.c_str());
                ConnectionStatus removed;
                removed.isConnected = false;
                removed.errorMessage = "removed stale control socket";
                m_connectionStatusQueue.push(std::move(removed));
            }
        }
    }

    // 清除所有存在的wpa_supplicant进程
    std::string kill_cmd = "pkill -f \"wpa_supplicant.*-i " + m_iface_name + "\" 2>/dev/null || true";
    system(kill_cmd.c_str());

    // 延时等待进程退出
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 利用选定的配置文件启动wpa_supplicant
    std::string start_cmd;
    if (!ctrl_dir.empty()) {
        start_cmd = "wpa_supplicant -B -i " + m_iface_name + " -c " + conf + " -C " + ctrl_dir + " 2>/dev/null";
    } else {
        start_cmd = "wpa_supplicant -B -i " + m_iface_name + " -c " + conf + " 2>/dev/null";
    }

    // 执行启动命令
    int rc = system(start_cmd.c_str());
    if (rc != 0) {
        ConnectionStatus err;
        err.isConnected = false;
        err.errorMessage = "failed to start wpa_supplicant with conf: " + conf;
        m_connectionStatusQueue.push(std::move(err));
        return;
    }

    // Wait for wpa_supplicant to create control socket file: <ctrl_dir>/<iface>
    // ctrl_socket was prepared earlier
    // 检查socket文件是否存在（根据上一个步骤启动的wpa_supplicant进程创建）
    auto socket_exists = [&](const std::string &path) -> bool {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return false;
        return S_ISSOCK(st.st_mode);
    };

    // If we have a ctrl socket path we can wait for it to appear. Otherwise
    // skip waiting and rely on wpa_ctrl_open(m_iface_name) fallback.
    bool found = false;
    if (!ctrl_socket.empty()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (socket_exists(ctrl_socket)) {
                found = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // Re-open control interfaces: close existing then open new ones
    if (m_mon_if) {
        wpa_ctrl_detach(m_mon_if);
        wpa_ctrl_close(m_mon_if);
        m_mon_if = nullptr;
    }
    if (m_ctrl_if) {
        wpa_ctrl_close(m_ctrl_if);
        m_ctrl_if = nullptr;
    }

    // Try to open control socket using constructed socket path first, then fallback to iface name
    if (found) {
        m_ctrl_if = wpa_ctrl_open(ctrl_socket.c_str());
    }
    if (!m_ctrl_if) {
        // Fallback: try opening by iface name which may work depending on libwpa_ctrl
        m_ctrl_if = wpa_ctrl_open(m_iface_name.c_str());
    }
    if (m_ctrl_if) {
        if (found) {
            m_mon_if = wpa_ctrl_open(ctrl_socket.c_str());
        }
        if (!m_mon_if) {
            m_mon_if = wpa_ctrl_open(m_iface_name.c_str());
        }
        if (m_mon_if)
            wpa_ctrl_attach(m_mon_if);
    }

    ConnectionStatus done;
    done.isConnected = false;
    done.errorMessage = "switch complete; network manager restarted";
    m_connectionStatusQueue.push(std::move(done));
}
