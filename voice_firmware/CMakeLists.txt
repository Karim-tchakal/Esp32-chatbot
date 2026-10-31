cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(chatbot_voice)

spiffs_create_partition_image(spiffs "${CMAKE_CURRENT_LIST_DIR}/../data" FLASH_IN_PROJECT)
