cmake_minimum_required(VERSION 3.16)

set(COMPONENT_DIRS "$ENV{IDF_PATH}/components" "${CMAKE_CURRENT_LIST_DIR}/main")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32c3_srun)

spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
