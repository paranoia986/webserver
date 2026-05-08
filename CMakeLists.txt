cmake_minimum_required(VERSION 3.15)
project(webserver CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IO_URING "Use io_uring instead of epoll" ON)

if(IO_URING)
    message(STATUS "Building with io_uring support")
    add_compile_definitions(IO_URING=1)
else()
    message(STATUS "Building with legacy epoll support")
    add_compile_definitions(IO_URING=0)
endif()

# 处理 DEBUG ?= 1 的逻辑 (Debug 对应 -g, Release 对应 -O2)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug) # 默认为 Debug
endif()

# 定义源码列表
set(SOURCES
    src/main.cpp
    src/webserver.cpp
    src/timer/lst_timer.cpp
    src/http/http_conn.cpp
    src/log/log.cpp
    src/CGImysql/sql_connection_pool.cpp
    src/YAMLparser/parser.cpp
)

add_executable(server ${SOURCES})

target_include_directories(server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 链接库 (对应 -lpthread -lmysqlclient)
find_package(Threads REQUIRED)
find_package(libmysqlclient REQUIRED)
find_package(yaml-cpp REQUIRED)

if(IO_URING)
    find_package(liburing REQUIRED)
endif()


target_link_libraries(server PRIVATE 
    Threads::Threads 
    libmysqlclient::libmysqlclient
    yaml-cpp::yaml-cpp
)

if(IO_URING)
    target_link_libraries(server PRIVATE liburing::liburing)
endif()

file(COPY "${CMAKE_SOURCE_DIR}/src/root" DESTINATION "${CMAKE_BINARY_DIR}")
file(COPY "${CMAKE_SOURCE_DIR}/src/resource" DESTINATION "${CMAKE_BINARY_DIR}")

install(TARGETS server DESTINATION "."
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        )

install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/root" 
        DESTINATION "bin" 
)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/resource" 
        DESTINATION "bin" 
)