cmake_minimum_required(VERSION 3.13)

project(c2-control)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
find_package(jansson CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)

add_executable(c2-control ${SOURCE_FILES})

target_link_libraries(c2-control PRIVATE jansson::jansson CURL::libcurl_static)
