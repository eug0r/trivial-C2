cmake_minimum_required(VERSION 3.13)

project(c2-server)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
find_package(jansson CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
#to use your own libuuid installation comment
#the next line and the respective linking line
find_package(unofficial-libuuid REQUIRED)


add_executable(c2-server ${SOURCE_FILES})

target_link_libraries(c2-server PRIVATE jansson::jansson)
target_link_libraries(c2-server PRIVATE SQLite::SQLite3)
#if libuuid is installed on your system
#target_link_libraries(c2-server PRIVATE uuid)
#if it isn't:
target_link_libraries(c2-server PRIVATE unofficial::UUID::uuid)
