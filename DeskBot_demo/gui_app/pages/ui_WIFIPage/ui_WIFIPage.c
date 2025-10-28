#include <string.h>
#include "ui_WIFIPage.h"
// [修改] 包含单一的、新的 C-API 头文件
#include "app_WIFIPage/app_wifi_service.h"

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

// [修改] 只有一个轮询计时器
static lv_timer_t * wifi_poll_timer; 

static lv_obj_t * ui_ConnectingModal = NULL; // 用于 "正在连接..." 的模态框


///////////////////// 静态函数 (FUNCTIONS) ////////////////////
static void wifi_ssid_click_cb(lv_event_t * e);

// [新增] 轮询辅助函数: 处理状态更新
static void ui_update_wifi_status(void)
{
    app_wifi_connection_status_t status;
    char details[64];

    // 1. 检查 C-API 是否有新状态
    if (app_wifi_get_new_status(&status, details, sizeof(details)))
    {
        LV_LOG_USER("New status detected: %d", status);

        // 2. [CRASH FIX] 处理模态框 (模态框是全局的, 可以在任何页面关闭)
        if (status == APP_WIFI_STATUS_CONNECTED || 
            status == APP_WIFI_STATUS_CONNECTION_FAILED || 
            status == APP_WIFI_STATUS_DISCONNECTED) 
        {
            if (ui_ConnectingModal) 
            {
                lv_obj_del(ui_ConnectingModal);
                ui_ConnectingModal = NULL;
            }
        }

        // 3. 根据状态更新 UI
        switch(status) {
            case APP_WIFI_STATUS_SCANNING:
                LV_LOG_USER("UI update: Scanning...");
                // (UI 已在 scan_button_event_handler 中设置)
                break;
                
            case APP_WIFI_STATUS_SCAN_FINISHED:
                LV_LOG_USER("UI update: Scan Finished.");
                // [CRASH FIX] 只有在列表页活动时才操作列表页的控件
                if (lv_scr_act() == ui_WIFIRootMenu) {
                    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_state(ui_BtnScan, LV_STATE_DISABLED);
                    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scan");
                }
                break;
                
            case APP_WIFI_STATUS_CONNECTING:
                LV_LOG_USER("UI update: Connecting...");
                break;
                
            case APP_WIFI_STATUS_CONNECTED:
                LV_LOG_USER("UI update: Connected.");
                ui_msgbox_info("Success", "Wi-Fi Connected!");
                // [CRASH FIX] 只有在密码页(的父对象)活动时才尝试返回
                if (ui_TextAreaPassword && lv_scr_act() == lv_obj_get_parent(ui_TextAreaPassword)) {
                    lv_lib_pm_OpenPrePage(&page_manager); // 成功, 关闭密码页
                }
                break;
                
            case APP_WIFI_STATUS_CONNECTION_FAILED:
                LV_LOG_USER("UI update: Connection Failed.");
                ui_msgbox_info("Error", details[0] ? details : "Connection Failed");
                // 失败, 停留在密码页
                break;
                
            case APP_WIFI_STATUS_DISCONNECTED:
                LV_LOG_USER("UI update: Disconnected.");
                break;
                
            default:
                break;
        }
    }
}

// [新增] 轮询辅助函数: 处理扫描列表更新
static void ui_update_scan_list(void)
{
    // [CRASH FIX] 只有在 Wi-Fi 列表页是活动屏幕时才执行
    if (lv_scr_act() != ui_WIFIRootMenu) {
        return; 
    }

    // 假设最多 32 个 AP
    app_wifi_scan_result_t results[32];
    int count = 32;

    // 1. 检查 C-API 是否有新列表
    if (app_wifi_get_new_scan_results(results, &count))
    {
        LV_LOG_USER("New scan list detected. Populating %d items.", count);
        
        // 2. 清理旧列表
        lv_obj_clean(ui_WIFIList);

        // 3. 填充真实扫描结果
        for (int i = 0; i < count; i++)
        {
            lv_obj_t * btn = lv_list_add_btn(ui_WIFIList, LV_SYMBOL_WIFI, results[i].ssid);
            lv_obj_add_event_cb(btn, wifi_ssid_click_cb, LV_EVENT_CLICKED, NULL);
        }
    }
}


/**
 * @brief [修改] LVGL 定时器回调, 用于轮询C后端
 */
static void wifi_poll_timer_cb(lv_timer_t * timer) {
    // 1. 告诉 C++ 后端处理内部事件
    app_wifi_poll();
    
    // 2. 从 C++ 后端拉取状态并更新 UI
    ui_update_wifi_status();
    
    // 3. 从 C++ 后端拉取列表并更新 UI
    ui_update_scan_list();
}

/**
 * @brief [修改] Wi-Fi 主列表页的返回按钮处理器
 * 这是退出 Wi-Fi "App" 的唯一出口, 在这里清理服务
 */
static void wifi_main_back_event_handler(lv_event_t * e)
{
    LV_LOG_USER("Exiting Wi-Fi, de-initializing service.");

    // [修改] 停止计时器并反初始化C++后端
    if(wifi_poll_timer) {
        lv_timer_del(wifi_poll_timer);
        wifi_poll_timer = NULL;
    }
    app_wifi_deinit();
    
    // 调用原始的 "back" 逻辑
    lv_lib_pm_OpenPrePage(&page_manager);
}

/**
 * @brief 通用的返回按钮事件处理器
 */
static void back_event_handler(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    lv_obj_t * page_now = lv_event_get_user_data(e);
    
    // 退出此页面
    lv_lib_pm_OpenPrePage(&page_manager);
}


// --- [移除] 移除所有 on_scan_finished 和 on_status_changed 回调 ---
// --- [移除] 移除所有 LV_USE_SIMULATOR == 0/1 的条件编译块 ---


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

    // 4. [修改] 调用 C-API
    LV_LOG_USER("Calling app_wifi_request_scan().");
    app_wifi_request_scan();
    // 结果将通过 wifi_poll_timer_cb 异步返回
}

/**
 * @brief [不变] 显示 "正在连接..." 模态框
 */
static void ui_show_connecting_modal(void)
{
    if(ui_ConnectingModal) {
        lv_obj_del(ui_ConnectingModal);
        ui_ConnectingModal = NULL;
    }
    
    lv_obj_t * mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN); 
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    ui_ConnectingModal = lv_obj_create(mask);
    lv_obj_set_size(ui_ConnectingModal, 200, 100);
    lv_obj_align(ui_ConnectingModal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ui_ConnectingModal, lv_color_white(), 0);
    lv_obj_set_style_radius(ui_ConnectingModal, 8, 0);

    lv_obj_t * spinner = lv_spinner_create(ui_ConnectingModal);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t * label = lv_label_create(ui_ConnectingModal);
    lv_label_set_text(label, "Connecting...");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 30);
    
    ui_ConnectingModal = mask;
}

/**
 * @brief [不变] 列表中某个 SSID 被点击时的事件
 */
static void wifi_ssid_click_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * btn = lv_event_get_target(e);
    const char * ssid = lv_list_get_btn_text(ui_WIFIList, btn);

    if(code == LV_EVENT_CLICKED) {
        if (!ssid) return;
        LV_LOG_USER("Selected SSID: %s", ssid);

        // 1. 保存被选中的 SSID
        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        selected_ssid[sizeof(selected_ssid) - 1] = '\0';

        // 2. 打开密码输入子页面
        lv_lib_pm_OpenPage(&page_manager, NULL, "WIFIPasswordMenu");
    }
}

/**
 * @brief [修改] 虚拟键盘事件 (用于密码页面)
 */
static void keyboard_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * kb = lv_event_get_target(e);

    if (code == LV_EVENT_READY) { // "OK" 按钮被按下
        const char * password = lv_textarea_get_text(ui_TextAreaPassword);
        LV_LOG_USER("Attempting to connect to %s", selected_ssid);

        // 弹出 "Connecting..." 模态框以阻止重复点击
        ui_show_connecting_modal();
        
        // [修改] --- 调用 C API ---
        app_wifi_connect(selected_ssid, password);
        // 结果将通过 wifi_poll_timer_cb 异步返回
        
    } else if (code == LV_EVENT_CANCEL) { // "Close" 按钮被按下
        // 关闭键盘和密码页，返回到 Wi-Fi 列表
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

///////////////////// 子屏幕 (sub screens) ////////////////////

static void ui_WIFIPasswordMenu_init(void)
{
    lv_obj_t * ui_WIFIPasswordMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIPasswordMenu, LV_OBJ_FLAG_SCROLLABLE);

    // [修改] 确保轮询计时器在子页面也存在
    // (计时器由 ui_WIFIPage_init 创建, 在 wifi_main_back_event_handler 销毁)
    if(!wifi_poll_timer) {
        LV_LOG_WARN("wifi_poll_timer not found in sub-page! Creating one.");
        wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, NULL);
    }

    // 1. 返回按钮
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
    lv_obj_set_size(ui_Keyboard, 320, 150);
    lv_obj_set_align(ui_Keyboard, LV_ALIGN_BOTTOM_MID);
    lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaPassword); 
    lv_obj_add_event_cb(ui_Keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);

    // 加载页面
    lv_scr_load_anim(ui_WIFIPasswordMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


static void ui_WIFIPasswordMenu_deinit(void)
{
    // [修改] 不再在这里删除计时器
    // 计时器是全局的, 由主返回按钮 (wifi_main_back_event_handler) 管理
    
    // 页面管理器会自动处理 ui_WIFIPasswordMenu 对象的删除
    ui_TextAreaPassword = NULL; // 清除静态指针
}

///////////////// 子屏幕页面管理器 (不变) /////////////////

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

    // 1. 返回按钮 (绑定到新的清理 handler)
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIRootMenu);
    lv_obj_set_width(ui_BtnBack, 50);
    lv_obj_set_height(ui_BtnBack, 45);
    lv_obj_set_x(ui_BtnBack, 5);
    lv_obj_set_y(ui_BtnBack, 0);
    lv_obj_set_style_bg_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BtnBack, 64, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_BtnBack, wifi_main_back_event_handler, LV_EVENT_CLICKED, ui_WIFIRootMenu);

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


    // --- [修改] ---
    // 5. 初始时触发一次扫描
    LV_LOG_USER("Initializing Wi-Fi C-API.");
    
    // 1. 初始化服务
    // [NEXUS] 确认 "wlan0" 是你的接口
    if (app_wifi_init("wlan0") != 0) { 
        LV_LOG_ERROR("Failed to init Wi-Fi service");
    }
    
    // 2. [移除] 移除回调注册
    
    // 3. 创建轮询定时器 (确保唯一)
    if(wifi_poll_timer) {
        lv_timer_del(wifi_poll_timer);
    }
    wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, NULL); // 100ms
    
    // 4. 初始时触发一次扫描
    scan_button_event_handler(NULL); 
    // --- [END MODIFICATION] ---

    // 加载页面
    lv_scr_load_anim(ui_WIFIRootMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


/////////////////// 屏幕 (SCREEN) deinit ////////////////////

void ui_WIFIPage_deinit()
{
    // [修改]
    // 真正的清理逻辑在 wifi_main_back_event_handler 中, 因为那是此 "App" 的
    // 唯一出口。如果你的页面管理器 (PM) 在切换页面时调用此 deinit,
    // 我们不希望在这里停止 C++ 后端。
    
    // 但是, 我们必须清理 LVGL 计时器, 否则它会尝试调用一个
    // 存在于已销毁页面(ui_WIFIRootMenu)上下文中的回调。
    
    // 安全起见: 假设此函数在页面被销毁时调用, 我们必须删除计时器。
    // `wifi_main_back_event_handler` 也会尝试删除它,
    // (lv_timer_del 如果 timer == NULL 会安全返回)

    if(wifi_poll_timer) {
        LV_LOG_USER("ui_WIFIPage_deinit: Deleting poll timer.");
        lv_timer_del(wifi_poll_timer);
        wifi_poll_timer = NULL;
    }
    
    // C++ 后端 (app_wifi_deinit) 由 wifi_main_back_event_handler 显式调用
    
    // 清理静态指针
    ui_WIFIRootMenu = NULL;
    ui_WIFIList = NULL;
    ui_BtnScan = NULL;
    ui_SpinnerScan = NULL;
}