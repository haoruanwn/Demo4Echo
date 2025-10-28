/*
 * 文件名: wifi_service.c
 * 描述: 使用 wpa_ctrl 和 pthread 实现的纯 C Wi-Fi 服务
 * 作者: Nexus (为用户构建)
 */

#include "app_WIFIPage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h> 
#include "wpa_ctrl.h"

// 调试日志
#define WIFI_LOG(fmt, ...) printf("[WIFI_SVC] " fmt "\n", ##__VA_ARGS__)

// wpa_ctrl 连接
static struct wpa_ctrl *g_ctrl_cmd = NULL;     // 用于发送命令 (LVGL 线程)
static struct wpa_ctrl *g_ctrl_monitor = NULL; // 用于接收事件 (Monitor 线程)
static char g_iface_path[128];                 // e.g., "/var/run/wpa_supplicant/wlan0"

// 线程管理
static pthread_t g_monitor_thread_id;
static volatile int g_monitor_running = 0;

// 线程间通信 (事件标志)
static pthread_mutex_t g_event_mutex;
static volatile int g_event_scan_finished = 0;
static volatile wifi_connection_status_t g_event_conn_status = WIFI_STATUS_IDLE;
static char g_event_status_details[64] = {0};

// LVGL 回调
static wifi_scan_cb_t g_scan_cb = NULL;
static wifi_status_cb_t g_status_cb = NULL;

// --- 内部辅助函数 ---

/**
 * @brief (LVGL 线程) 向 wpa_supplicant 发送命令并获取回复
 */
static int _wifi_service_send_cmd(const char *cmd, char *reply_buf, size_t reply_buf_len)
{
    if (!g_ctrl_cmd) return -1;

    size_t len = reply_buf_len - 1;
    int ret = wpa_ctrl_request(g_ctrl_cmd, cmd, strlen(cmd), reply_buf, &len, NULL);
    if (ret == 0) {
        reply_buf[len] = '\0';
    } else {
        strcpy(reply_buf, "ERROR");
    }
    // WIFI_LOG("CMD: '%s' -> REPLY: '%s'", cmd, reply_buf);
    return ret;
}

/**
 * @brief (LVGL 线程) 解析 SCAN_RESULTS 的输出
 */
static void _parse_scan_results(char *scan_data, wifi_scan_cb_t callback)
{
    // 假设最多 32 个 AP
    wifi_scan_result_t results[32];
    int count = 0;

    char *line = strtok(scan_data, "\n");
    // 跳过第一行 (表头)
    if (line) line = strtok(NULL, "\n"); 

    while (line != NULL && count < 32)
    {
        wifi_scan_result_t *res = &results[count];
        
        // 解析格式: bssid / frequency / signal level / flags / ssid
        int ret = sscanf(line, "%17s\t%*d\t%d\t%127[^\t]\t%63[^\n]", 
                         res->bssid, 
                         &res->signal_level, 
                         res->flags, 
                         res->ssid);
        
        if (ret == 4) {
            count++;
        }
        line = strtok(NULL, "\n");
    }

    if (callback && count > 0) {
        callback(results, count);
    }
}

/**
 * @brief (Monitor 线程) 监视器线程的主循环
 */
static void* _wifi_monitor_thread_loop(void *arg)
{
    char buf[2048];
    size_t len;

    // 1. 附加到 wpa_supplicant 事件
    if (wpa_ctrl_attach(g_ctrl_monitor) != 0) {
        WIFI_LOG("Failed to attach to wpa_monitor");
        wpa_ctrl_close(g_ctrl_monitor);
        g_ctrl_monitor = NULL;
        return NULL;
    }
    WIFI_LOG("Monitor thread attached.");

    g_monitor_running = 1;
    while (g_monitor_running) 
    {
        // 2. 检查是否有待处理事件
        if (wpa_ctrl_pending(g_ctrl_monitor) > 0) {
            len = sizeof(buf) - 1;
            // 3. 接收事件 (阻塞)
            if (wpa_ctrl_recv(g_ctrl_monitor, buf, &len) == 0) {
                buf[len] = '\0';
                
                // 4. 解析事件并设置标志 (线程安全)
                pthread_mutex_lock(&g_event_mutex);

                if (strstr(buf, WPA_EVENT_SCAN_RESULTS)) {
                    WIFI_LOG("Event: Scan finished");
                    g_event_scan_finished = 1;
                } 
                else if (strstr(buf, WPA_EVENT_CONNECTED)) {
                    WIFI_LOG("Event: Connected");
                    g_event_conn_status = WIFI_STATUS_CONNECTED;
                } 
                else if (strstr(buf, WPA_EVENT_DISCONNECTED)) {
                    WIFI_LOG("Event: Disconnected");
                    g_event_conn_status = WIFI_STATUS_DISCONNECTED;
                }
                else if (strstr(buf, "WRONG_KEY") || strstr(buf, WPA_EVENT_AUTH_REJECT)) {
                    WIFI_LOG("Event: Connection Failed (Auth)");
                    g_event_conn_status = WIFI_STATUS_CONNECTION_FAILED;
                    strcpy(g_event_status_details, "Wrong Password");
                }
                // ... 可以添加更多事件, e.g., WPA_EVENT_ASSOC_REJECT ...
                
                pthread_mutex_unlock(&g_event_mutex);
            }
        } else {
            // 避免忙循环
            usleep(200000); // 200ms
        }
    }

    // 5. 清理
    wpa_ctrl_detach(g_ctrl_monitor);
    WIFI_LOG("Monitor thread detached.");
    return NULL;
}


// --- 公开的 API 函数实现 ---

int wifi_service_init(const char *iface_name)
{
    // e.g., "/var/run/wpa_supplicant/wlan0"
    snprintf(g_iface_path, sizeof(g_iface_path), "/var/run/wpa_supplicant/%s", iface_name);

    // 1. 初始化互斥锁
    pthread_mutex_init(&g_event_mutex, NULL);
    g_monitor_running = 0;
    g_event_scan_finished = 0;
    g_event_conn_status = WIFI_STATUS_IDLE;

    // 2. 打开命令连接
    g_ctrl_cmd = wpa_ctrl_open(g_iface_path);
    if (!g_ctrl_cmd) {
        WIFI_LOG("Failed to open wpa_ctrl (cmd): %s", g_iface_path);
        return -1;
    }

    // 3. 打开监视器连接
    g_ctrl_monitor = wpa_ctrl_open(g_iface_path);
    if (!g_ctrl_monitor) {
        WIFI_LOG("Failed to open wpa_ctrl (monitor): %s", g_iface_path);
        wpa_ctrl_close(g_ctrl_cmd);
        return -1;
    }

    // 4. 启动监视器线程
    if (pthread_create(&g_monitor_thread_id, NULL, _wifi_monitor_thread_loop, NULL) != 0) {
        WIFI_LOG("Failed to create monitor thread");
        wpa_ctrl_close(g_ctrl_cmd);
        wpa_ctrl_close(g_ctrl_monitor);
        return -1;
    }

    WIFI_LOG("Wi-Fi Service Initialized.");
    return 0;
}

void wifi_service_deinit(void)
{
    if (g_monitor_running) {
        g_monitor_running = 0;
        // (可能需要更鲁棒的机制来唤醒阻塞的 wpa_ctrl_recv)
        pthread_join(g_monitor_thread_id, NULL);
        WIFI_LOG("Monitor thread joined.");
    }

    if (g_ctrl_cmd) wpa_ctrl_close(g_ctrl_cmd);
    if (g_ctrl_monitor) wpa_ctrl_close(g_ctrl_monitor);
    
    pthread_mutex_destroy(&g_event_mutex);
    
    g_ctrl_cmd = NULL;
    g_ctrl_monitor = NULL;
    WIFI_LOG("Wi-Fi Service Deinitialized.");
}

void wifi_service_register_callbacks(wifi_scan_cb_t scan_cb, wifi_status_cb_t status_cb)
{
    g_scan_cb = scan_cb;
    g_status_cb = status_cb;
}

void wifi_service_request_scan(void)
{
    char reply[32];
    _wifi_service_send_cmd("SCAN", reply, sizeof(reply));
    // 立即在 LVGL 线程中设置状态
    if (g_status_cb) {
        g_status_cb(WIFI_STATUS_SCANNING, NULL);
    }
}

void wifi_service_connect(const char *ssid, const char *psk)
{
    char cmd[256];
    char reply[64];
    
    // 立即在 LVGL 线程中设置状态
    if (g_status_cb) {
        g_status_cb(WIFI_STATUS_CONNECTING, NULL);
    }

    // 1. 添加网络
    _wifi_service_send_cmd("ADD_NETWORK", reply, sizeof(reply));
    int net_id = atoi(reply); // reply 是 "0\n" 或 "1\n" ...
    if (net_id < 0) {
        WIFI_LOG("Failed to add network");
        if(g_status_cb) g_status_cb(WIFI_STATUS_CONNECTION_FAILED, "ADD_NETWORK Failed");
        return;
    }

    // 2. 设置 SSID
    snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", net_id, ssid);
    _wifi_service_send_cmd(cmd, reply, sizeof(reply));

    // 3. 设置密码 (或开放网络)
    if (psk && psk[0] != '\0') {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", net_id, psk);
        _wifi_service_send_cmd(cmd, reply, sizeof(reply));
    } else {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", net_id);
        _wifi_service_send_cmd(cmd, reply, sizeof(reply));
    }

    // 4. 启用并选择网络
    snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", net_id);
    _wifi_service_send_cmd(cmd, reply, sizeof(reply));
    
    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", net_id);
    _wifi_service_send_cmd(cmd, reply, sizeof(reply));
}

void wifi_service_disconnect(void)
{
    char reply[32];
    _wifi_service_send_cmd("DISCONNECT", reply, sizeof(reply));
}

void wifi_service_poll(void)
{
    // 缓冲区 (用于从 wpa_supplicant 获取大数据)
    // 静态分配, 避免在轮询中占用栈空间
    static char reply_buf[4096]; 

    // --- 1. 检查事件标志 (线程安全) ---
    pthread_mutex_lock(&g_event_mutex);
    
    int scan_flag = g_event_scan_finished;
    wifi_connection_status_t status_flag = g_event_conn_status;
    char details[64];
    strcpy(details, g_event_status_details);

    // 清除已处理的标志
    g_event_scan_finished = 0;
    g_event_conn_status = WIFI_STATUS_IDLE; // 重置为空闲
    g_event_status_details[0] = '\0';
    
    pthread_mutex_unlock(&g_event_mutex);
    
    // --- 2. 在 LVGL 线程中处理事件 ---

    // 2.1 处理扫描结果
    if (scan_flag && g_scan_cb) {
        WIFI_LOG("Polling: Scan flag detected. Getting results...");
        // 扫描完成了, 现在我们同步获取结果
        if (_wifi_service_send_cmd("SCAN_RESULTS", reply_buf, sizeof(reply_buf)) == 0) {
            _parse_scan_results(reply_buf, g_scan_cb);
        }
        // 通知 UI 扫描已结束
        if (g_status_cb) {
            g_status_cb(WIFI_STATUS_SCAN_FINISHED, NULL);
        }
    }
    
    // 2.2 处理状态变化
    if (status_flag != WIFI_STATUS_IDLE && g_status_cb) {
        WIFI_LOG("Polling: Status flag detected: %d", status_flag);
        g_status_cb(status_flag, details[0] ? details : NULL);
        
        // **关键**: 如果连接成功, 触发 DHCP
        if (status_flag == WIFI_STATUS_CONNECTED) {
            WIFI_LOG("Triggering DHCP client...");
            // 这是在嵌入式 Linux C 中最简单、最常见的做法
            // -n: 失败后退出, -q: 安静, -b: 后台
            system("udhcpc -i wlan0 -n -q -b &");
        }
    }
}