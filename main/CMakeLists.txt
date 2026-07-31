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
    SRCS "wifi_service.cpp" "main.cpp" "peripherals.cpp"
         "drivers/audio/audio_driver.cpp"
         "drivers/audio/audio_ulog_recorder.cpp"
         "drivers/sdcard/sdcard_driver.cpp"
         "drivers/camera/camera_driver.cpp"
         "drivers/system_monitor/system_monitor.cpp"
         "logger/logger.cpp"
         "phone_app_camera.cpp" "phone_app_audio.cpp"
         "phone_app_music.cpp" "phone_app_settings.cpp"
         "camera_stream.cpp"
         "web_config_server.cpp"
         "ppa_preprocessor.cpp"
         "generated/fps_stats.cpp"
         "generated/wifi_state.cpp"
         "generated/audio_level.cpp"
         "generated/audio_frame.cpp"
         "generated/camera_state.cpp"
         "generated/recording_state.cpp"
         "generated/volume_state.cpp"
         "generated/ulog_state.cpp"
         "generated/system_stats.cpp"
         "generated/system_alert.cpp"
         "generated/camera_frame_chunk.cpp"
         "generated/uORBTopics.cpp"
    INCLUDE_DIRS "." "generated" "drivers/audio" "drivers/sdcard" "drivers/camera" "drivers/system_monitor" "logger"
    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c esp_driver_ppa fatfs
             shine_encoder nvs_flash esp_wifi esp_video
             esp_new_jpeg mdns cjson esp_http_server example_video_common
             esp_audio_simple_player uorb ulog wifi_manager captive_dns
    PRIV_REQUIRES esp_driver_i2s esp_driver_sdmmc)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# ─────────────────────────────────────────────────────────────────
#  ESP-Claw IM Platform: conditional dependency
#  When any IM channel is enabled, link cap_im_platform so main.cpp
#  can include its headers and call its APIs.
# ─────────────────────────────────────────────────────────────────
if(CONFIG_APP_CLAW_CAP_IM_WECHAT OR CONFIG_APP_CLAW_CAP_IM_FEISHU OR
   CONFIG_APP_CLAW_CAP_IM_QQ OR CONFIG_APP_CLAW_CAP_IM_TG OR
   CONFIG_APP_CLAW_CAP_IM_LOCAL)
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        idf::cap_im_platform
        idf::claw_core
        idf::claw_cap
        idf::claw_event_router
        idf::claw_manager
        idf::claw_memory
        idf::claw_skill
        idf::cap_llm_config
        idf::cap_session_mgr
        idf::cap_scheduler
        # Phase 1: Tool capabilities
        idf::cap_system
        idf::cap_files
        idf::cap_http_request
        # Phase 1: MCP server/client + mDNS discovery
        idf::cap_mcp_server
        idf::cap_mcp_client
        idf::mcp_mdns
        # Phase 2: Extended capabilities
        idf::cap_lua
        idf::cap_web_search
        idf::cap_agent_mgr
        idf::cap_router_mgr
        idf::cap_skill_mgr
        idf::cap_llm_inspect
        idf::claw_kv_nvs
    )
    # Device-specific MCP tools (depends on claw + peripheral drivers)
    target_sources(${COMPONENT_LIB} PRIVATE "device_mcp_tools.cpp")
endif()

if(CONFIG_APP_CLAW_CAP_IM_LOCAL)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::cap_im_local)
endif()

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

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})