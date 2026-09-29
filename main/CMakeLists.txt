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
    SRCS "wifi_service.cpp" "main.cpp" "peripherals.cpp" "board_i2c.c"
         "drivers/audio/audio_driver.cpp"
         "drivers/audio/audio_ulog_recorder.cpp"
         "drivers/sdcard/sdcard_driver.cpp"
         "drivers/camera/camera_driver.cpp"
         "drivers/system_monitor/system_monitor.cpp"
         "logger/logger.cpp"
         "camera_stream.cpp"
         "web_config_server.cpp"
         "ppa_preprocessor.cpp"
         "generated/fps_stats.cpp"
         "generated/wifi_state.cpp"
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
             espressif__esp_audio_codec nvs_flash esp_wifi esp_video
             esp_new_jpeg mdns cjson esp_http_server example_video_common
             esp_audio_simple_player uorb ulog wifi_manager captive_dns
    PRIV_REQUIRES esp_driver_i2s esp_driver_sdmmc)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})