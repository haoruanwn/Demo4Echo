// app/wifi_manager_sim.cpp
#include "wifi_manager_sim.hpp"
#include <thread> // for sleep_for

void SimWifiManager::requestScan() {
    currentStatus_ = WifiConnectionStatus::SCANNING;
    newStatusFlag_ = true;
    scanPending_ = true;
    scanTimer_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

void SimWifiManager::connect(const std::string& ssid, const std::string& psk) {
    currentStatus_ = WifiConnectionStatus::CONNECTING;
    newStatusFlag_ = true;
    connectPending_ = true;
    connectTimer_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    
    if (psk == "12345678") {
        statusDetails_ = ""; // 模拟成功
    } else {
        statusDetails_ = "Wrong Password"; // 模拟失败
    }
}

void SimWifiManager::disconnect() {
    currentStatus_ = WifiConnectionStatus::DISCONNECTED;
    newStatusFlag_ = true;
}

void SimWifiManager::poll() {
    auto now = std::chrono::steady_clock::now();

    if (scanPending_ && now >= scanTimer_) {
        scanPending_ = false;
        currentStatus_ = WifiConnectionStatus::SCAN_FINISHED;
        newStatusFlag_ = true;
        
        lastScanResults_ = {
            {"", -50, "[WPA2]", "MyHome_WIFI_5G (Sim)"},
            {"", -70, "[WPA2]", "Office_Network (Sim)"},
            {"", -85, "[OPEN]", "CoffeeShop_Free (Sim)"}
        };
        newScanFlag_ = true;
    }
    
    if (connectPending_ && now >= connectTimer_) {
        connectPending_ = false;
        if (statusDetails_ == "Wrong Password") {
            currentStatus_ = WifiConnectionStatus::CONNECTION_FAILED;
        } else {
            currentStatus_ = WifiConnectionStatus::CONNECTED;
        }
        newStatusFlag_ = true;
    }
}

bool SimWifiManager::getNewStatus(WifiConnectionStatus& status, std::string& details) {
    if (newStatusFlag_) {
        status = currentStatus_;
        details = statusDetails_;
        newStatusFlag_ = false;
        return true;
    }
    return false;
}

bool SimWifiManager::getNewScanResults(std::vector<WifiScanResult>& results) {
    if (newScanFlag_) {
        results = lastScanResults_;
        newScanFlag_ = false;
        return true;
    }
    return false;
}