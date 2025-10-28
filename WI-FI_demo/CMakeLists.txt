cmake_minimum_required(VERSION 3.16)

project(WifiManager LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE) # Enable compile command to ease indexing with e.g. clangd

find_path(WPA_CTRL_INCLUDE_DIR 
          NAMES wpa_ctrl.h
          # 在 sysroot 的 /usr/include/wpa_supplicant 中查找
          PATH_SUFFIXES wpa_supplicant 
)

find_library(WPA_CTRL_LIBRARY NAMES wpa_client)

if(NOT WPA_CTRL_INCLUDE_DIR OR NOT WPA_CTRL_LIBRARY)
    message(FATAL_ERROR "Could not find wpa_client library or headers in sysroot: ${CMAKE_SYSROOT}")
else()
    message(STATUS "Found wpa_client includes: ${WPA_CTRL_INCLUDE_DIR}")
    message(STATUS "Found wpa_client library: ${WPA_CTRL_LIBRARY}")
endif()

add_library(wpa_supplicant::wpa_client UNKNOWN IMPORTED)
set_target_properties(wpa_supplicant::wpa_client PROPERTIES
    IMPORTED_LOCATION "${WPA_CTRL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WPA_CTRL_INCLUDE_DIR}"
)

add_library(${PROJECT_NAME} STATIC
    src/WifiManager.cc
    src/WifiManager_C_Wrapper.cc
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        wpa_supplicant::wpa_client
)
