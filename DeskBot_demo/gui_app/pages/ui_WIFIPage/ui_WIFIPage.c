#include <string.h>
#include "ui_WIFIPage.h"
#include "app_wifi_service.h" // 包含新的 C-API 头文件

///////////////////// 变量 (VARIABLES) ////////////////////

lv_obj_t * ui_WIFIRootMenu;
lv_obj_t * ui_WIFIList;
lv_obj_t * ui_BtnScan;
lv_obj_t * ui_SpinnerScan;

static char selected_ssid[64];
lv_obj_t * ui_TextAreaPassword;
lv_obj_t * ui_Keyboard;

// [修改] 轮询计时器是全局的(相对于此 App)
static lv_timer_t * wifi_poll_timer; 

static lv_obj_t * ui_ConnectingModal = NULL;


///////////////////// 静态函数 (FUNCTIONS) ////////////////////
static void wifi_ssid_click_cb(lv_event_t * e);
static void wifi_main_back_event_handler(lv_event_t * e); // [新增] 前向声明

// [新增] 轮询辅助函数: 处理状态更新
static void ui_update_wifi_status(void)
{
    app_wifi_connection_status_t status;
    char details[64];

    if (app_wifi_get_new_status(&status, details, sizeof(details)))
    {
        LV_LOG_USER("New status detected: %d", status);

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

        switch(status) {
            case APP_WIFI_STATUS_SCANNING:
                LV_LOG_USER("UI update: Scanning...");
                break;
                
            case APP_WIFI_STATUS_SCAN_FINISHED:
                LV_LOG_USER("UI update: Scan Finished.");
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
                
                // [修改] 关键修复: 连接成功后, 退出整个 Wi-Fi App
                // 而不是返回列表页
                LV_LOG_USER("Connection successful, exiting Wi-Fi app.");
                wifi_main_back_event_handler(NULL); // 触发主返回(清理)逻辑
                break;
                
            case APP_WIFI_STATUS_CONNECTION_FAILED:
                LV_LOG_USER("UI update: Connection Failed.");
                ui_msgbox_info("Error", details[0] ? details : "Connection Failed");
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
    if (lv_scr_act() != ui_WIFIRootMenu) {
        return; 
    }

    app_wifi_scan_result_t results[32];
    int count = 32;

    if (app_wifi_get_new_scan_results(results, &count))
    {
        LV_LOG_USER("New scan list detected. Populating %d items.", count);
        
        lv_obj_clean(ui_WIFIList);
        if (count == 0) {
            lv_list_add_text(ui_WIFIList, "No networks found");
        } else {
            for (int i = 0; i < count; i++)
            {
                lv_obj_t * btn = lv_list_add_btn(ui_WIFIList, LV_SYMBOL_WIFI, results[i].ssid);
                lv_obj_add_event_cb(btn, wifi_ssid_click_cb, LV_EVENT_CLICKED, NULL);
            }
        }
    }
}


/**
 * @brief [修改] LVGL 定时器回调, 用于轮询C后端
 */
static void wifi_poll_timer_cb(lv_timer_t * timer) {
    app_wifi_poll();
    ui_update_wifi_status();
    ui_update_scan_list();
}

/**
 * @brief [修改] Wi-Fi 主列表页的返回按钮处理器
 */
static void wifi_main_back_event_handler(lv_event_t * e)
{
    LV_LOG_USER("Exiting Wi-Fi, de-initializing service.");

    // [修改] 这是唯一的清理点
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
    
    lv_lib_pm_OpenPrePage(&page_manager);
}

/**
 * @brief "Scan" 按钮点击事件
 */
static void scan_button_event_handler(lv_event_t * e)
{
    if(e) {
        lv_event_code_t code = lv_event_get_code(e);
        if(code != LV_EVENT_CLICKED) {
            return;
        }
    }

    LV_LOG_USER("Wi-Fi scan triggered.");

    lv_obj_clean(ui_WIFIList);
    lv_list_add_text(ui_WIFIList, "Scanning..."); // 提示用户
    lv_obj_add_state(ui_BtnScan, LV_STATE_DISABLED);
    lv_label_set_text(lv_obj_get_child(ui_BtnScan, 0), "Scanning...");
    lv_obj_clear_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);

    LV_LOG_USER("Calling app_wifi_request_scan().");
    app_wifi_request_scan();
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
        if (!ssid || strcmp(ssid, "Scanning...") == 0 || strcmp(ssid, "No networks found") == 0) {
             return; // 忽略无效点击
        }
        LV_LOG_USER("Selected SSID: %s", ssid);

        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        selected_ssid[sizeof(selected_ssid) - 1] = '\0';

        lv_lib_pm_OpenPage(&page_manager, NULL, "WIFIPasswordMenu");
    }
}

/**
 * @brief [修改] 虚拟键盘事件
 */
static void keyboard_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * kb = lv_event_get_target(e);

    if (code == LV_EVENT_READY) {
        const char * password = lv_textarea_get_text(ui_TextAreaPassword);
        LV_LOG_USER("Attempting to connect to %s", selected_ssid);

        ui_show_connecting_modal();
        app_wifi_connect(selected_ssid, password);
        
    } else if (code == LV_EVENT_CANCEL) {
        lv_lib_pm_OpenPrePage(&page_manager);
    }
}

///////////////////// 子屏幕 (sub screens) ////////////////////

static void ui_WIFIPasswordMenu_init(void)
{
    lv_obj_t * ui_WIFIPasswordMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIPasswordMenu, LV_OBJ_FLAG_SCROLLABLE);

    // [修改] 不再创建定时器. 依赖 ui_WIFIPage_init 创建的定时器
    if(!wifi_poll_timer) {
        // 这是一个错误状态, 但我们还是创建一个以防万一
        LV_LOG_ERROR("wifi_poll_timer not found in sub-page! This shouldn't happen.");
        wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, NULL);
    }

    // 1. 返回按钮
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIPasswordMenu);
    lv_obj_set_width(ui_BtnBack, 50);
    lv_obj_set_height(ui_BtnBack, 45);
    // ... (样式不变)
    lv_obj_set_style_bg_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ui_BtnBack, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BtnBack, 64, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui_BtnBack, back_event_handler, LV_EVENT_CLICKED, ui_WIFIPasswordMenu);

    lv_obj_t * ui_LabelBack = lv_label_create(ui_BtnBack);
    lv_label_set_text(ui_LabelBack, "");
    lv_obj_set_style_text_font(ui_LabelBack, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 2. 标题
    lv_obj_t * ui_LabelSSIDTitle = lv_label_create(ui_WIFIPasswordMenu);
    // ... (样式不变)
    lv_obj_set_width(ui_LabelSSIDTitle, 260);
    lv_obj_set_align(ui_LabelSSIDTitle, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_LabelSSIDTitle, 15);
    lv_label_set_text_fmt(ui_LabelSSIDTitle, "SSID: %s", selected_ssid);
    lv_label_set_long_mode(ui_LabelSSIDTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ui_LabelSSIDTitle, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);

    // 3. 密码输入框
    ui_TextAreaPassword = lv_textarea_create(ui_WIFIPasswordMenu);
    // ... (样式不变)
    lv_obj_set_width(ui_TextAreaPassword, 300);
    lv_obj_set_height(ui_TextAreaPassword, 40);
    lv_obj_set_align(ui_TextAreaPassword, LV_ALIGN_TOP_MID);
    lv_obj_set_y(ui_TextAreaPassword, 50);
    lv_textarea_set_placeholder_text(ui_TextAreaPassword, "Password...");
    lv_textarea_set_one_line(ui_TextAreaPassword, true);
    lv_textarea_set_password_mode(ui_TextAreaPassword, true);

    // 4. 虚拟键盘
    ui_Keyboard = lv_keyboard_create(ui_WIFIPasswordMenu);
    // ... (样式不变)
    lv_obj_set_size(ui_Keyboard, 320, 150);
    lv_obj_set_align(ui_Keyboard, LV_ALIGN_BOTTOM_MID);
    lv_keyboard_set_textarea(ui_Keyboard, ui_TextAreaPassword); 
    lv_obj_add_event_cb(ui_Keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);

    lv_scr_load_anim(ui_WIFIPasswordMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


static void ui_WIFIPasswordMenu_deinit(void)
{
    // [修改] 不再删除计时器
    LV_LOG_USER("WIFIPasswordMenu deinit");
    ui_TextAreaPassword = NULL; 
    ui_Keyboard = NULL;
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

void ui_WIFIPage_init(void)
{
    static bool inited = false;
    if(inited == false)
    {
        _ui_wifi_sub_menus_creat();
        LV_LOG_USER("WIFIPage sub menus created.");
        inited = true;
    }

    ui_WIFIRootMenu = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_WIFIRootMenu, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 返回按钮 (绑定到清理 handler)
    lv_obj_t * ui_BtnBack = lv_button_create(ui_WIFIRootMenu);
    // ... (样式不变)
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
    // ... (样式不变)
    lv_obj_set_width(ui_BtnScan, 80);
    lv_obj_set_height(ui_BtnScan, 40);
    lv_obj_set_align(ui_BtnScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_BtnScan, -10);
    lv_obj_set_y(ui_BtnScan, 5);
    lv_obj_add_event_cb(ui_BtnScan, scan_button_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * ui_LabelScan = lv_label_create(ui_BtnScan);
    lv_label_set_text(ui_LabelScan, "Scan");
    lv_obj_set_align(ui_LabelScan, LV_ALIGN_CENTER);

    // 3. 扫描“菊花”
    ui_SpinnerScan = lv_spinner_create(ui_WIFIRootMenu);
    // ... (样式不变)
    lv_obj_set_size(ui_SpinnerScan, 24, 24);
    lv_obj_set_align(ui_SpinnerScan, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_x(ui_SpinnerScan, -100);
    lv_obj_set_y(ui_SpinnerScan, 13);
    lv_obj_add_flag(ui_SpinnerScan, LV_OBJ_FLAG_HIDDEN);

    // 4. Wi-Fi 列表
    ui_WIFIList = lv_list_create(ui_WIFIRootMenu);
    lv_obj_set_size(ui_WIFIList, 310, 180);
    lv_obj_set_align(ui_WIFIList, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_y(ui_WIFIList, -5);


    // --- [修改] ---
    // 5. 初始时触发一次扫描
    LV_LOG_USER("Initializing Wi-Fi C-API.");
    
    // 1. 初始化服务
    if (app_wifi_init("wlan0") != 0) { 
        LV_LOG_ERROR("Failed to init Wi-Fi service");
    }
    
    // 2. 创建轮询定时器 (确保唯一)
    if(wifi_poll_timer) {
        lv_timer_del(wifi_poll_timer);
    }
    wifi_poll_timer = lv_timer_create(wifi_poll_timer_cb, 100, NULL);
    
    // 4. 初始时触发一次扫描
    scan_button_event_handler(NULL); 
    // --- [END MODIFICATION] ---

    lv_scr_load_anim(ui_WIFIRootMenu, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 100, 0, true);
}


/////////////////// 屏幕 (SCREEN) deinit ////////////////////

void ui_WIFIPage_deinit()
{
    // [修改]
    // 当页面管理器销毁此页面(例如切换到密码页时)
    // 我们不再删除定时器或 C++ 服务
    // 这些的清理工作完全由 wifi_main_back_event_handler 负责
    LV_LOG_USER("ui_WIFIPage_deinit: (Skipping timer/service deinit)");
    
    // 清理静态指针
    ui_WIFIRootMenu = NULL;
    ui_WIFIList = NULL;
    ui_BtnScan = NULL;
    ui_SpinnerScan = NULL;
}