# ─────────────────────────────────────────────────────────────────
#  Git metadata at build time (commit hash + message + branch)
# ─────────────────────────────────────────────────────────────────
execute_process(
    COMMAND git log -1 "--format=%h %s"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_LOG1
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_LOG1}" STREQUAL "")
    set(PROJECT_GIT_LOG1 "unknown")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_COMMIT}" STREQUAL "")
    set(PROJECT_GIT_COMMIT "unknown")
endif()

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_BRANCH}" STREQUAL "")
    set(PROJECT_GIT_BRANCH "unknown")
endif()

execute_process(
    COMMAND git log -1 "--format=%an <%ae>"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_AUTHOR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_AUTHOR}" STREQUAL "")
    set(PROJECT_GIT_AUTHOR "unknown")
endif()

execute_process(
    COMMAND git log -1 --format=%ai
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_DATE}" STREQUAL "")
    set(PROJECT_GIT_DATE "unknown")
endif()

execute_process(
    COMMAND git log -1 --format=%s
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_GIT_MSG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if("${PROJECT_GIT_MSG}" STREQUAL "")
    set(PROJECT_GIT_MSG "unknown")
endif()

# Generate git_info.h header file
# Escape backslashes and quotes for C string literals
string(REPLACE "\\" "\\\\" PROJECT_GIT_LOG1 "${PROJECT_GIT_LOG1}")
string(REPLACE "\"" "\\\"" PROJECT_GIT_LOG1 "${PROJECT_GIT_LOG1}")
string(REPLACE "\\" "\\\\" PROJECT_GIT_MSG "${PROJECT_GIT_MSG}")
string(REPLACE "\"" "\\\"" PROJECT_GIT_MSG "${PROJECT_GIT_MSG}")
string(REPLACE "\\" "\\\\" PROJECT_GIT_AUTHOR "${PROJECT_GIT_AUTHOR}")
string(REPLACE "\"" "\\\"" PROJECT_GIT_AUTHOR "${PROJECT_GIT_AUTHOR}")
string(REPLACE "\\" "\\\\" PROJECT_GIT_DATE "${PROJECT_GIT_DATE}")
string(REPLACE "\"" "\\\"" PROJECT_GIT_DATE "${PROJECT_GIT_DATE}")

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/git_info.h
"#pragma once\n"
"#define GIT_LOG1   \"${PROJECT_GIT_LOG1}\"\n"
"#define GIT_COMMIT \"${PROJECT_GIT_COMMIT}\"\n"
"#define GIT_BRANCH \"${PROJECT_GIT_BRANCH}\"\n"
"#define GIT_AUTHOR \"${PROJECT_GIT_AUTHOR}\"\n"
"#define GIT_DATE   \"${PROJECT_GIT_DATE}\"\n"
"#define GIT_MSG    \"${PROJECT_GIT_MSG}\"\n"
)

idf_component_register(
    SRCS "main.cpp" "peripherals.cpp"
         "drivers/audio/audio_driver.cpp"
         "drivers/sdcard/sdcard_driver.cpp"
         "drivers/camera/camera_driver.cpp"
         "drivers/system_monitor/system_monitor.cpp"
         "logger/logger.cpp"
         "phone_app_camera.cpp" "phone_app_audio.cpp"
         "phone_app_music.cpp" "phone_app_settings.cpp"
         "phone_app_camera_stream.cpp" "camera_stream.cpp"
         "web_config_server.cpp"
         "generated/fps_stats.cpp"
         "generated/detection_result.cpp"
         "generated/wifi_state.cpp"
         "generated/audio_level.cpp"
         "generated/camera_state.cpp"
         "generated/recording_state.cpp"
         "generated/volume_state.cpp"
         "generated/ulog_state.cpp"
         "generated/system_stats.cpp"
         "generated/system_alert.cpp"
         "generated/uORBTopics.cpp"
    INCLUDE_DIRS "." "generated" "drivers/audio" "drivers/sdcard" "drivers/camera" "drivers/system_monitor" "logger"
    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c fatfs
             shine_encoder nvs_flash esp_wifi coco_detect esp_video
             esp_new_jpeg mdns cjson esp_http_server example_video_common
             esp_audio_simple_player uorb ulog
    PRIV_REQUIRES esp_driver_i2s esp_driver_sdmmc)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# Get LVGL component library to suppress warnings
function(get_component_library component_name output_var)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

get_component_library("lvgl" LVGL_LIB)
target_compile_options(${LVGL_LIB} PRIVATE "-Wno-attributes")

# ─────────────────────────────────────────────────────────────────
#  ESP-DL mbedtls compat: ESP-IDF v6.x (mbedtls 4.x) moved
#  mbedtls/sha256.h to mbedtls/private/.  main/compat/mbedtls/
#  provides a forwarding shim until esp-dl upstream catches up.
# ─────────────────────────────────────────────────────────────────
get_component_library("espressif__esp-dl" ESP_DL_LIB)
target_include_directories(${ESP_DL_LIB} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})