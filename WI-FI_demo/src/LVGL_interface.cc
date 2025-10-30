// 入口，在此进行 LVGL 线程与 WifiManager 的交互，并且进行WifiManager的模板实例化

// (在你的主程序入口或初始化函数中)
#include "./inc/WifiManager.h"
#include <memory>

// 这是你的全局宏
#ifdef LV_USE_SIMULATOR
    // 编译时选择模拟器策略
    using MyWifiManager = WifiManager<SimulatorWifiStrategy>;
#else
    // 编译时选择真实策略
    using MyWifiManager = WifiManager<RealWifiStrategy>;
#endif

// 全局实例
std::unique_ptr<MyWifiManager> g_wifiManager;

void InitializeApp() {
    // 编译器在这里已经决定了是创建 WifiManager<SimulatorWifiStrategy> 
    // 还是 WifiManager<RealWifiStrategy>。
    g_wifiManager = std::make_unique<MyWifiManager>();
}