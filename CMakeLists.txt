cmake_minimum_required(VERSION 3.16)
project(wardrivingmapper_pi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# LVGL locates lv_conf.h via the include path (include/lv_conf.h).
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE=1)

include_directories(
  ${CMAKE_SOURCE_DIR}/include          # lv_conf.h
  ${CMAKE_SOURCE_DIR}/lib/lvgl         # lvgl.h
  ${CMAKE_SOURCE_DIR}/lib/lvgl/src     # extra/libs/qrcode/lv_qrcode.h
  ${CMAKE_SOURCE_DIR}/src/platform     # wdm_platform.h, wdm_tick.h
  ${CMAKE_SOURCE_DIR}/src/ui           # screens.h, strutil.h
  ${CMAKE_SOURCE_DIR}/third_party      # stb_image_write.h
)

file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/lvgl/src/*.c)

set(APP_SRC
  src/main.c
  src/ui/screens.c
  src/ui/strutil.c
  src/platform/platform_stub.c
)

add_executable(wdm_ui ${APP_SRC} ${LVGL_SRC})
target_link_libraries(wdm_ui m)
