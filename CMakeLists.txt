cmake_minimum_required(VERSION 3.1)
project(PingTools)

# 第三方库
add_subdirectory(3rd/Moe.Core)
add_subdirectory(3rd/Moe.UV)

# 编译选项
set(CMAKE_CXX_STANDARD 11)

if(MSVC)
    add_definitions(-D_WIN32_WINNT=0x0600 -D_GNU_SOURCE -D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /utf-8")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -Wextra -Wno-implicit-fallthrough")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wall -Wextra -Wno-implicit-fallthrough")
endif()

# 目标
add_executable(PingServer src/Server.cpp)
target_link_libraries(PingServer MoeCore MoeUV)

add_executable(PingClient src/Client.cpp)
target_link_libraries(PingClient MoeCore MoeUV)
