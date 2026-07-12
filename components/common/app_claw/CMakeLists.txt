set(app_claw_srcs
    "src/app_claw.c"
    "src/app_capabilities.c"
    "src/app_lua_modules.c"
)

if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND app_claw_srcs "src/app_claw_cli.c")
endif()

idf_component_register(
    SRCS
        ${app_claw_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        cap_im_platform
)
