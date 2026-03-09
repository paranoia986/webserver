cmake_minimum_required(VERSION 3.15)
project(webserver CXX)

# 处理 DEBUG ?= 1 的逻辑 (Debug 对应 -g, Release 对应 -O2)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug) # 默认为 Debug
endif()

# 定义源码列表
set(SOURCES
    src/main.cpp
    src/webserver.cpp
    src/config.cpp
    src/timer/lst_timer.cpp
    src/http/http_conn.cpp
    src/log/log.cpp
    src/CGImysql/sql_connection_pool.cpp
)

add_executable(server ${SOURCES})

# 链接库 (对应 -lpthread -lmysqlclient)
find_package(Threads REQUIRED)
find_package(libmysqlclient REQUIRED)

target_link_libraries(server PRIVATE 
    Threads::Threads 
    libmysqlclient::libmysqlclient
)

add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/src/root"
    "$<TARGET_FILE_DIR:server>/root"
    COMMENT "正在同步网页资源到输出目录..."
)

install(TARGETS server DESTINATION "."
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        )

install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/root" 
        DESTINATION "bin" 
)
