idf_component_register(
    SRCS "main.cpp" "phone_app_camera.cpp" "phone_app_audio.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_mm esp_driver_isp esp_driver_cam esp_driver_i2c fatfs
    PRIV_REQUIRES esp_driver_sdmmc)

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

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
target_compile_options(${LVGL_LIB} PUBLIC "-Wno-attributes")
