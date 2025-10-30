#include "../inc/RealWifiStrategy.h"
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <thread>
#include <vector>
#include <wpa_ctrl.h>

// 构造函数必须提供路径与接口名称
RealWifiStrategy::RealWifiStrategy(const std::string &ctrlPath, const std::string &ifaceName,
                                   const std::string &wpaConfApp, const std::string &wpaConfDev) :
    m_ctrl_path(ctrlPath), m_iface_name(ifaceName), m_wpa_conf_app(wpaConfApp), m_wpa_conf_dev(wpaConfDev) {
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

void RealWifiStrategy::WorkerLoop() {
    // Try to open control interfaces. We try the common socket path first, then
    // fall back to interface name. If opening fails we still process queued tasks
    // (they will fail early with an error).
    // Common control path used by many systems:
    const char *ctrl_path = "/var/run/wpa_supplicant/wlan0";
    m_ctrl_if = wpa_ctrl_open(ctrl_path);
    if (!m_ctrl_if) {
        // fallback to interface name (some wpa_ctrl implementations accept this)
        m_ctrl_if = wpa_ctrl_open("wlan0");
    }

    // Try to open a monitor interface for events
    m_mon_if = nullptr;
    if (m_ctrl_if) {
        m_mon_if = wpa_ctrl_open(ctrl_path);
        if (!m_mon_if) {
            m_mon_if = wpa_ctrl_open("wlan0");
        }
        if (m_mon_if) {
            // Attach to receive events where supported. Ignore return value.
            wpa_ctrl_attach(m_mon_if);
        }
    }

    // Process tasks from the queue. Thread blocks inside pop(); if queue is
    // closed pop() returns nullopt and we exit.
    while (!m_stopWorker.load()) {
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
            return;
        }
    }

    ConnectionStatus s;
    s.isConnected = false;
    s.errorMessage = "connection timeout or failed";
    m_connectionStatusQueue.push(std::move(s));
}

void RealWifiStrategy::DoSwitchNetwork(bool toAppNetwork) {
    // A simple implementation might enable/disable a saved network id.
    // Here we provide a best-effort: if toAppNetwork==true try to select
    // network with ssid "APP_AP" (this is application specific). Otherwise,
    // do nothing and report status via connection queue.
    ConnectionStatus s;
    s.isConnected = false;
    if (!m_ctrl_if) {
        s.errorMessage = "no ctrl interface";
        m_connectionStatusQueue.push(std::move(s));
        return;
    }

    // Build command to stop any existing wpa_supplicant for the interface
    const std::string &conf = toAppNetwork ? m_wpa_conf_app : m_wpa_conf_dev;
    // Notify caller that switching is starting
    s.errorMessage = std::string("switching to ") + (toAppNetwork ? "app" : "dev");
    m_connectionStatusQueue.push(s);

    // Stop wpa_supplicant for this interface. Command may vary by system.
    std::string kill_cmd = "pkill -f \"wpa_supplicant.*-i " + m_iface_name + "\" 2>/dev/null || true";
    system(kill_cmd.c_str());
    // small pause to let process exit
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Start wpa_supplicant with desired conf. Use -B to background when supported.
    std::string start_cmd =
            "wpa_supplicant -B -i " + m_iface_name + " -c " + conf + " -C /var/run/wpa_supplicant 2>/dev/null";
    int rc = system(start_cmd.c_str());
    if (rc != 0) {
        ConnectionStatus err;
        err.isConnected = false;
        err.errorMessage = "failed to start wpa_supplicant with conf: " + conf;
        m_connectionStatusQueue.push(std::move(err));
        return;
    }

    // Allow wpa_supplicant to create control socket
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

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

    // Try to open control socket using configured ctrl path or interface
    m_ctrl_if = wpa_ctrl_open(m_ctrl_path.c_str());
    if (!m_ctrl_if) {
        m_ctrl_if = wpa_ctrl_open(m_iface_name.c_str());
    }
    if (m_ctrl_if) {
        m_mon_if = wpa_ctrl_open(m_ctrl_path.c_str());
        if (!m_mon_if)
            m_mon_if = wpa_ctrl_open(m_iface_name.c_str());
        if (m_mon_if)
            wpa_ctrl_attach(m_mon_if);
    }

    ConnectionStatus done;
    done.isConnected = false;
    done.errorMessage = "switch complete; network manager restarted";
    m_connectionStatusQueue.push(std::move(done));
}
