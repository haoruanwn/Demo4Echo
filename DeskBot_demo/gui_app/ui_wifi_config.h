/* Central Wi‑Fi configuration header
 * - Declares C-visible config strings so `ui.c` (C) can define them
 * - Provides a default compile-time LV_USE_SIMULATOR value which can be
 *   overridden by the build system or other headers before including this
 *   file.
 */
#ifndef UI_WIFI_CONFIG_H
#define UI_WIFI_CONFIG_H

/* Compile-time selector: if not defined elsewhere, default to 0 (real)
 * You can override this from build flags (-DLV_USE_SIMULATOR=1) or by
 * defining it before including this header.
 */
#ifndef LV_USE_SIMULATOR
#define LV_USE_SIMULATOR 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* C-friendly config strings (defined in ui.c). Use these from C code.
 * C++ code may convert them to std::string when needed.
 */
extern const char * g_wifi_ctrlPath_c;
extern const char * g_wifi_iface_c;
extern const char * g_wpa_conf_app_c;
extern const char * g_wpa_conf_dev_c;

#ifdef __cplusplus
}
#endif

#endif // UI_WIFI_CONFIG_H
