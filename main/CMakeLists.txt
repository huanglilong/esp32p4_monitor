idf_component_register(
    SRCS "main.cpp" "peripherals.cpp"
         "drivers/audio/audio_driver.cpp"
         "drivers/sdcard/sdcard_driver.cpp"
         "drivers/camera/camera_driver.cpp"
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
         "generated/uORBTopics.cpp"
    INCLUDE_DIRS "." "generated" "drivers/audio" "drivers/sdcard" "drivers/camera"
    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c fatfs
             shine_encoder nvs_flash esp_wifi coco_detect esp_video
             esp_new_jpeg mdns cjson esp_http_server example_video_common
             esp_audio_simple_player uorb ulog
    PRIV_REQUIRES esp_driver_i2s esp_driver_sdmmc)

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
