#include "../inc/RealWifiStrategy.h"

RealWifiStrategy::RealWifiStrategy() {
    // TODO: open wpa_ctrl interfaces, start worker thread
}

RealWifiStrategy::~RealWifiStrategy() {
    // TODO: stop worker thread and clean up
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
    // User to implement: open wpa_ctrl, attach, poll task/event fds, process tasks and wpa events
}

void RealWifiStrategy::HandleWpaEvents() {
    // User to implement: recv events and push to m_connectionStatusQueue / call DoGetScanResults
}

void RealWifiStrategy::DoScanRequest() {
    // User to implement: call wpa_ctrl_request(m_ctrl_if, "SCAN", ...)
}

void RealWifiStrategy::DoGetScanResults() {
    // User to implement: request SCAN_RESULTS and parse into WifiScanResult then push to m_scanResultQueue
}

void RealWifiStrategy::DoConnectRequest(const std::string &ssid, const std::string &psk) {
    // User to implement: add/set/select/enable network via wpa_ctrl_request
}

void RealWifiStrategy::DoSwitchNetwork(bool toAppNetwork) {
    // User to implement
}
