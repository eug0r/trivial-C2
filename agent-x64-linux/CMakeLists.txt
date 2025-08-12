cmake_minimum_required(VERSION 3.13)

project(c2-agent)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23)
find_package(jansson CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

add_executable(c2-agent ${SOURCE_FILES})

target_link_libraries(c2-agent PRIVATE jansson::jansson CURL::libcurl_static)
