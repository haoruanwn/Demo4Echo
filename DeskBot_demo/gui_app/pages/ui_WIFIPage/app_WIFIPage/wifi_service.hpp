// app/wifi_service.hpp
#ifndef _WIFI_SERVICE_HPP
#define _WIFI_SERVICE_HPP

#include "iwifi_manager.hpp"
#include <memory>

class WifiService {
public:
    static WifiService& getInstance() {
        static WifiService instance;
        return instance;
    }

    void setManager(std::unique_ptr<IWifiManager> manager) {
        manager_ = std::move(manager);
    }
    
    IWifiManager* getManager() {
        return manager_.get();
    }
    
    void init() {
        if(manager_) manager_->init();
    }
    
    void deinit() {
        if(manager_) manager_->deinit();
        manager_.reset(); // 销毁
    }

private:
    WifiService() = default;
    ~WifiService() = default;
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    std::unique_ptr<IWifiManager> manager_;
};

#endif

// app/wifi_service.cpp
// (这个文件目前是空的, 因为单例实现都在头文件中了)