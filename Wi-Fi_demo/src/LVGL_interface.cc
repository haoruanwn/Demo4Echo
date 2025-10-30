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

// 入口，在此进行 LVGL 线程与 WifiManager 的交互，并且进行 WifiManager 的模板实例化
void InitializeApp() {
#ifdef LV_USE_SIMULATOR
	// simulator 无需额外参数
	auto &wifiManager = MyWifiManager::GetInstance();
#else
	// 对于真实策略，必须在初始化时显式提供控制 socket 路径、接口名、app conf 与 dev conf
	const std::string ctrlPath = "/var/run/wpa_supplicant/wlan0";
	const std::string ifaceName = "wlan0";
	const std::string wpaConfApp = "/etc/wpa_supplicant_app.conf";
	const std::string wpaConfDev = "/etc/wpa_supplicant_dev.conf";
	auto &wifiManager = MyWifiManager::GetInstance(ctrlPath, ifaceName, wpaConfApp, wpaConfDev);
#endif
}
