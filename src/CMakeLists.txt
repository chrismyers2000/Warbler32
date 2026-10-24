# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       PRIV_REQUIRES esp_wifi esp_http_client esp-tls spiffs)

# esp_roaming.h is private to esp_wifi, so expose its directory to main for
# the roaming config API used by wifi_manager.c.
idf_component_get_property(esp_wifi_dir esp_wifi COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
                           "${esp_wifi_dir}/wifi_apps/roaming_app/include")
