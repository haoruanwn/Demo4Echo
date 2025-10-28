#include <string.h> // for strcmp
#include "ui_WIFIPage.h"
#if LV_USE_SIMULATOR == 0
    #include "app_WIFIPage.h" 
#endif
// --- [END MODIFICATION] ---


///////////////////// 变量 (VARIABLES) ////////////////////

// --- Wi-Fi 列表页面 ---
lv_obj_t * ui_WIFIRootMenu;      // 页面根对象
lv_obj_t * ui_WIFIList;          // Wi-Fi 列表
lv_obj_t * ui_BtnScan;           // 扫描按钮
lv_obj_t * ui_SpinnerScan;       // 扫描中的“菊花”

// --- Wi-Fi 密码页面 ---
static char selected_ssid[64];   // 用于在页面间传递SSID
lv_obj_t * ui_TextAreaPassword;  // 密码输入框
lv_obj_t * ui_Keyboard;          // 虚拟键盘

// --- [MODIFICATION] ---
// 条件变量: 模拟器用一个, 真实硬件用另一个
#if LV_USE_SIMULATOR == 1
    static lv_timer_t * scan_sim_timer; // 用于模拟扫描的计时器
#else
    static lv_timer_t * wifi_poll_timer; // 用于轮询 C 后端的计时器
#endif
// --- [END MODIFICATION] ---


///////////////////// 静态函数 (FUNCTIONS) ////////////////////
static void wifi_ssid_click_cb(lv_event_t * e);

/**
 * @brief 通用的返回按钮事件处理器
 * (从 ui_SettingPage.c 借鉴)
 */
static void back_event_handler(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    lv_obj_t * page_now = lv_event_get_user_data(e);
    
    // 退出此页面
    lv_lib_pm_OpenPrePage(&page_manager);
}


// --- [NEW] 真实硬件的回调函数 ---
#if LV_USE_SIMULATOR == 0

/**
 * @brief [REAL] 扫描完成的回调 (来自 wifi_service)
 * 由 wifi_service_poll() 在 LVGL 线程中调用
 */
static void on_scan_finished(wifi_scan_result_t *results, int count)
{
    LV_LOG_USER("Wi-Fi scan finished (Real).");
    // 1. 清理旧列表
    lv_obj_clean(ui_WIFIList);

    // 2. 填充真实扫描结果
    for (int i = 0; i < count; i++)
    {
        // (可选) 你可以在此根据 results[i].signal_level 
        // 选择不同的 Wi-Fi 图标 (例如 强/中/弱)
        lv_obj_t * btn = lv_list_add_btn(ui_WIFIList, LV_SYMBOL_WIFI, results[i].ssid);
        
        // 传递 NULL, 在回调中我们使用 lv_list_get_btn_text 获取
        lv_obj_add_event_cb(btn, wifi_ssid_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

/**
 * @brief [REAL] Wi-Fi 状态改变的回调 (来自 wifi_service)
 * 由 wifi_service_poll() 在 LVGL 线程中调用
 */
static void on_status_changed(wifi_connection_status_t status, const char *details)
{
    switch(status) {
        case WIFI_STATUS_SCANNING:
            LV_LOG_USER("Real status: Scanning...");
            // UI 已经在 "Scanning..." 状态
            break;
            
        case WIFI_STATUS_SCAN_FINISHED:
            LV_LOG_USER("Real status: Scan Finished.");
            // 隐藏“菊花”并重新启用扫描按钮
            lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(ui_BtnScan, LV_STATE_DISABLED);
            lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scan");
            break;
            
        case WIFI_STATUS_CONNECTING:
            LV_LOG_USER("Real status: Connecting...");
            // (可选) 可以在此弹出一个 "Connecting..." 的非阻塞提示
            break;
            
        case WIFI_STATUS_CONNECTED:
            LV_LOG_USER("Real status: Connected.");
            ui_msgbox_info("Success", "Wi-Fi Connected!");
            lv_lib_pm_OpenPrePage(&page_manager); // 成功, 关闭密码页
            break;
            
        case WIFI_STATUS_CONNECTION_FAILED:
            LV_LOG_USER("Real status: Connection Failed.");
            ui_msgbox_info("Error", details ? details : "Connection Failed");
            // 失败, 停留在密码页
            break;
            
        case WIFI_STATUS_DISCONNECTED:
            LV_LOG_USER("Real status: Disconnected.");
            // (可选: 显示断连提示)
            break;
            
        default:
            break;
    }
}

/**
 * @brief [REAL] LVGL 定时器回调, 用于轮询C后端
 */
static void wifi_poll_timer_cb(lv_timer_t * timer) {
    wifi_service_poll();
}

#endif // LV_USE_SIMULATOR == 0
// --- [END NEW] ---


#if LV_USE_SIMULATOR == 1
/**
 * @brief (模拟) 扫描完成的回调
 */
static void scan_finished_cb(lv_timer_t * timer)
{
    LV_LOG_USER("Wi-Fi scan finished (simulation).");

    // 1. 隐藏“菊花”并重新启用扫描按钮
    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(ui_BtnScan, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scan");

    // 2. 清理旧列表
    lv_obj_clean(ui_WIFIList);

    // 3. 添加模拟的扫描结果
    // --- 模拟数据 ---
    const char* fake_ssids[] = {"MyHome_WIFI_5G", "Office_Network", "Guest_WIFI", "CoffeeShop_Free", NULL};
    // --- 模拟数据结束 ---
    
    for (int i = 0; fake_ssids[i] != NULL; i++)
    {
        lv_obj_t * btn = lv_list_add_btn(ui_WIFIList, LV_SYMBOL_WIFI, fake_ssids[i]);
        // [MODIFICATION] 传递 NULL, 保持与真实 API 一致
        lv_obj_add_event_cb(btn, wifi_ssid_click_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_timer_del(scan_sim_timer); // 删除一次性计时器
    scan_sim_timer = NULL;
}
#endif // LV_USE_SIMULATOR == 1


/**
 * @brief "Scan" 按钮点击事件
 */
static void scan_button_event_handler(lv_event_t * e)
{
    // 1. 检查 'e' (事件)
    if(e) {
        lv_event_code_t code = lv_event_get_code(e);
        if(code != LV_EVENT_CLICKED) {
            return;
        }
    }

    LV_LOG_USER("Wi-Fi scan triggered.");

    // 1. 清空当前列表
    lv_obj_clean(ui_WIFIList);

    // 2. 禁用按钮，修改文本为 "Scanning..."
    lv_obj_add_state(ui_BtnScan, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scanning...");

    // 3. 显示“菊花”
    lv_obj_clear_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);

    // --- [MODIFICATION] ---
    // 4. 根据模式调用不同逻辑
    #if LV_USE_SIMULATOR == 1
        // 模拟一个2秒的扫描延迟
        LV_LOG_USER("Using SIMULATOR scan.");
        if(scan_sim_timer) lv_timer_del(scan_sim_timer);
        scan_sim_timer = lv_timer_create(scan_finished_cb, 2000, NULL);
    #else
        // 调用真实的 C API
        LV_LOG_USER("Using REAL wifi_service scan.");
        wifi_service_request_scan();
        // 结果将通过 on_status_changed(WIFI_STATUS_SCAN_FINISHED) 异步返回
    #endif
    // --- [END MODIFICATION] ---
}

/**
 * @brief 列表中某个 SSID 被点击时的事件
 */
static void wifi_ssid_click_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    // --- [MODIFICATION] ---
    // 从按钮 target 获取文本, 而不是 user_data, 这样更健壮
    lv_obj_t * btn = lv_event_get_target(e);
    const char * ssid = lv_list_get_btn_text(ui_WIFIList, btn);
    // --- [END MODIFICATION] ---

    if(code == LV_EVENT_CLICKED) {
        if (!ssid) return;
        LV_LOG_USER("Selected SSID: %s", ssid);

        // 1. 保存被选中的 SSID，以便密码页面可以显示它
        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        selected_ssid[sizeof(selected_ssid) - 1] = '\0';

        // 2. 打开密码输入子页面
        lv_lib_pm_OpenPage(&page_manager, NULL, "WIFIPasswordMenu");
    }
}

/**
 * @brief 虚拟键盘事件 (用于密码页面)
 */
static void keyboard_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * kb = lv_event_get_target(e);

    if (code == LV_EVENT_READY) { // "OK" 按钮被按下
        const char * password = lv_textarea_get_text(ui_TextAreaPassword);
        LV_LOG_USER("Attempting to connect to %s with password: %s", selected_ssid, password);

        // --- [MODIFICATION] ---
        #if LV_USE_SIMULATOR == 1
            LV_LOG_USER("Using SIMULATOR connect logic.");
            // --- 模拟连接逻辑 ---
            if (strcmp(password, "12345678") == 0) {
                // 模拟成功
                ui_msgbox_info("Success", "Wi-Fi Connected!"); // (借鉴ui_msgbox_info)
                // 成功后，关闭键盘和密码页，返回到 Wi-Fi 列表
                lv_lib_pm_OpenPrePage(&page_manager);
            } else {
                // 模拟失败
                ui_msgbox_info("Error", "Connection Failed!\nWrong Password."); //
            }
            // --- 模拟结束 ---
        #else
            LV_LOG_USER("Using REAL wifi_service connect.");
            // --- 调用真实 C API ---
            wifi_service_connect(selected_ssid, password);
            // 保持在当前页面
            // 结果将通过 on_status_changed 异步返回
            // (成功则关闭页面, 失败则显示 msgbox)
        #endif
        // --- [END MODIFICATION] ---

    } else if (code == LV_EVENT_CANCEL) { // "Close" 按钮被按下
        // 关闭键盘和密码页，返回到 Wi-Fi 列表
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

///////////////////// 子屏幕 (sub screens) ////////////////////

// ... (ui_WIFIPasswordMenu_init 及其内容保持不变) ...
static void ui_WIFIPasswordMenu_init(void)
{
    lv_obj_t * ui_WIFIPasswordMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIPasswordMenu, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 返回按钮 (借鉴 ui_SettingPage.c)
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIPasswordMenu);
    lv_obj_set_width(ui_BtnBack, 50);
    lv_obj_set_height(ui_BtnBack, 45);
    lv_obj_set_x(ui_BtnBack, 5);
    lv_obj_set_y(ui_BtnBack, 0);
    lv_obj_set_style_bg_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BtnBack, 64, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_BtnBack, back_event_handler, LV_EVENT_CLICKED, ui_WIFIPasswordMenu);

    lv_obj_t * ui_LabelBack = lv_label_create(ui_BtnBack);
    lv_label_set_text(ui_LabelBack, "");
    lv_obj_set_style_text_font(ui_LabelBack, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 2. 标题 (显示正在连接的 SSID)
    lv_obj_t * ui_LabelSSIDTitle = lv_label_create(ui_WIFIPasswordMenu);
    lv_obj_set_width(ui_LabelSSIDTitle, 260);
    lv_obj_set_align(ui_LabelSSIDTitle, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_LabelSSIDTitle, 15);
    lv_label_set_text_fmt(ui_LabelSSIDTitle, "SSID: %s", selected_ssid);
    lv_label_set_long_mode(ui_LabelSSIDTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ui_LabelSSIDTitle, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 3. 密码输入框
    ui_TextAreaPassword = lv_textarea_create(ui_WIFIPasswordMenu);
    lv_obj_set_width(ui_TextAreaPassword, 300);
    lv_obj_set_height(ui_TextAreaPassword, 40);
    lv_obj_set_align(ui_TextAreaPassword, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_TextAreaPassword, 50);
    lv_textarea_set_placeholder_text(ui_TextAreaPassword, "Password...");
    lv_textarea_set_one_line(ui_TextAreaPassword, true);
    lv_textarea_set_password_mode(ui_TextAreaPassword, true);

    // 4. 虚拟键盘
    ui_Keyboard = lv_keyboard_create(ui_WIFIPasswordMenu);
    lv_obj_set_size(ui_Keyboard, 320, 150); // 调整键盘大小以适应屏幕
    lv_obj_set_align(ui_Keyboard, LV_ALIGN_BOTTOM_MID);
    lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaPassword); // 关联键盘和输入框
    lv_obj_add_event_cb(ui_Keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // 加载页面
    lv_scr_load_anim(ui_WIFIPasswordMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


// ... (ui_WIFIPasswordMenu_deinit, _WIFI_SUB_MENU_NUMS, ui_wifi_sub_menu_apps, _ui_wifi_sub_menus_creat 保持不变) ...
static void ui_WIFIPasswordMenu_deinit(void)
{
    // deinit
    // 页面管理器会自动处理 ui_WIFIPasswordMenu 对象的删除
}

///////////////// 子屏幕页面管理器 /////////////////

#define _WIFI_SUB_MENU_NUMS 1

ui_app_data_t ui_wifi_sub_menu_apps[_WIFI_SUB_MENU_NUMS] = 
{
    {
        .name = "WIFIPasswordMenu",
        .init = ui_WIFIPasswordMenu_init,
        .deinit = ui_WIFIPasswordMenu_deinit,
        .page_obj = NULL
    }
};

static void _ui_wifi_sub_menus_creat(void)
{
    for(int i = 0; i < _WIFI_SUB_MENU_NUMS; i++)
    {
        lv_lib_pm_CreatePage(&page_manager, ui_wifi_sub_menu_apps[i].name, 
                             ui_wifi_sub_menu_apps[i].init, 
                             ui_wifi_sub_menu_apps[i].deinit, NULL);
    }    
}


///////////////////// 屏幕 (SCREEN) init ////////////////////

/**
 * @brief 初始化 Wi-Fi 主页面 (列表页)
 */
void ui_WIFIPage_init(void)
{
    // 确保子页面只被创建一次
    static bool inited = false;
    if(inited == false)
    {
        _ui_wifi_sub_menus_creat();
        LV_LOG_USER("WIFIPage sub menus created.");
        inited = true;
    }

    ui_WIFIRootMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIRootMenu, LV_OBJ_FLAG_SCROLLABLE);

    // ... (BtnBack, LabelBack, BtnScan, LabelScan, SpinnerScan, WIFIList 的创建保持不变) ...
    // 1. 返回按钮 (借鉴 ui_SettingPage.c)
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIRootMenu);
    lv_obj_set_width(ui_BtnBack, 50);
    lv_obj_set_height(ui_BtnBack, 45);
    lv_obj_set_x(ui_BtnBack, 5);
    lv_obj_set_y(ui_BtnBack, 0);
    lv_obj_set_style_bg_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BtnBack, 64, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_BtnBack, back_event_handler, LV_EVENT_CLICKED, ui_WIFIRootMenu);

    lv_obj_t * ui_LabelBack = lv_label_create(ui_BtnBack);
    lv_label_set_text(ui_LabelBack, "");
    lv_obj_set_style_text_font(ui_LabelBack, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 2. 扫描按钮
    ui_BtnScan = lv_button_create(ui_WIFIRootMenu);
    lv_obj_set_width(ui_BtnScan, 80);
    lv_obj_set_height(ui_BtnScan, 40);
    lv_obj_set_align(ui_BtnScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_BtnScan, -10);
    lv_obj_set_y(ui_BtnScan, 5);
    lv_obj_add_event_cb(ui_BtnScan, scan_button_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * ui_LabelScan = lv_label_create(ui_BtnScan);
    lv_label_set_text(ui_LabelScan, "Scan");
    lv_obj_set_align(ui_LabelScan, LV_ALIGN_CENTER);

    // 3. 扫描“菊花” (Spinner)
    ui_SpinnerScan = lv_spinner_create(ui_WIFIRootMenu);
    lv_obj_set_size(ui_SpinnerScan, 24, 24);
    lv_obj_set_align(ui_SpinnerScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_SpinnerScan, -100);
    lv_obj_set_y(ui_SpinnerScan, 13);
    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN); // 默认隐藏

    // 4. Wi-Fi 列表
    ui_WIFIList = lv_list_create(ui_WIFIRootMenu);
    lv_obj_set_size(ui_WIFIList, 310, 180);
    lv_obj_set_align(ui_WIFIList, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(ui_WIFIList, -5);


    // --- [MODIFICATION] ---
    // 5. 初始时触发一次扫描 (根据模式)
    #if LV_USE_SIMULATOR == 1
        LV_LOG_USER("Using SIMULATOR init scan.");
        scan_button_event_handler(NULL); // 模拟一次点击
    #else
        LV_LOG_USER("Using REAL wifi_service init.");
        // 1. 初始化服务
        // [NEXUS] 确认 "wlan0" 是你的接口
        if (wifi_service_init("wlan0") != 0) { 
            LV_LOG_ERROR("Failed to init Wi-Fi service");
            // (可以显示一个错误标签)
        }
        
        // 2. 注册回调
        wifi_service_register_callbacks(on_scan_finished, on_status_changed);
        
        // 3. 创建轮询定时器
        wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, NULL); // 100ms
        
        // 4. 初始时触发一次扫描
        // (调用 handler 来显示菊花、清空列表等)
        scan_button_event_handler(NULL); 
    #endif
    // --- [END MODIFICATION] ---

    // 加载页面
    lv_scr_load_anim(ui_WIFIRootMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


/////////////////// 屏幕 (SCREEN) deinit ////////////////////

void ui_WIFIPage_deinit()
{
    // --- [MODIFICATION] ---
    #if LV_USE_SIMULATOR == 1
        // 确保模拟计时器被删除
        if(scan_sim_timer) {
            lv_timer_del(scan_sim_timer);
            scan_sim_timer = NULL;
        }
    #else
        // 确保轮询计时器被删除
        if(wifi_poll_timer) {
            lv_timer_del(wifi_poll_timer);
            wifi_poll_timer = NULL;
        }
        // 关闭 C 后端服务
        wifi_service_deinit();
    #endif
    // --- [END MODIFICATION] ---
    
    // 页面管理器会自动删除 ui_WIFIRootMenu
}