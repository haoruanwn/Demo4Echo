#include <memory>
#include "./inc/RealWifiStrategy.h"
#include "./inc/SimulatorWifiStrategy.h"
#include "./inc/WifiManager.h"

#ifdef LV_USE_SIMULATOR
// 编译时选择模拟器策略
using MyWifiManager = WifiManager<SimulatorWifiStrategy>;
#else
// 编译时选择真实策略
using MyWifiManager = WifiManager<RealWifiStrategy>;
#endif

// 入口，在此进行 LVGL 线程与 WifiManager 的交互，并且进行WifiManager的模板实例化


void InitializeApp() { auto &wifiManager = MyWifiManager::GetInstance(); }
