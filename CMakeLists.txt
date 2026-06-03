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
  ${CMAKE_SOURCE_DIR}/src/pipeline     # config.h, pipeline.h, …
  ${CMAKE_SOURCE_DIR}/third_party      # stb_image_write.h
)

file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/lvgl/src/*.c)

# Portable pipeline logic — host-testable, built on every platform.
set(PIPELINE_PORTABLE_SRC
  src/pipeline/config.c
  src/pipeline/geo.c
  src/pipeline/hwid.c
  src/pipeline/recordfmt.c
  src/pipeline/scan_parse.c
)

# Hardware glue — gpsd / iw / libcurl. Linux only (the host screenshot build
# never calls run_live(), so these symbols are never referenced there).
set(PIPELINE_LINUX_SRC
  src/pipeline/pipeline.c
  src/pipeline/gps_gpsd.c
  src/pipeline/scan_iw.c
  src/pipeline/upload.c
  src/pipeline/setup_ap.c
)

set(APP_SRC
  src/main.c
  src/ui/screens.c
  src/ui/strutil.c
  src/platform/platform_stub.c
  ${PIPELINE_PORTABLE_SRC}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND APP_SRC ${PIPELINE_LINUX_SRC})
endif()

add_executable(wdm_ui ${APP_SRC} ${LVGL_SRC})
target_link_libraries(wdm_ui m)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(wdm_ui pthread gps curl)
endif()

# Host unit tests for the portable pipeline modules: ./build/wdm_tests
add_executable(wdm_tests tests/test_pipeline.c ${PIPELINE_PORTABLE_SRC})
target_link_libraries(wdm_tests m)
