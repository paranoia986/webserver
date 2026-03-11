#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <yaml-cpp/yaml.h>
#include "../webserver.h"

class app_config {
public:
    // Server
    //端口号
    int PORT;

    //日志写入方式
    int LOGWrite;

    //触发组合模式
    int TRIGMode;

    //listenfd触发模式
    int LISTENTrigmode;

    //connfd触发模式
    int CONNTrigmode;

    //优雅关闭链接
    int OPT_LINGER;

    //数据库连接池数量
    int sql_num;

    //线程池内的线程数量
    int thread_num;

    //是否关闭日志
    int close_log;

    //并发模型选择
    int actor_model;

    // Database
    std::string user;

    std::string password;

    std::string name;

    app_config(const std::string& filename);
    ~app_config() {};
    void parse_arg(int argc, char*argv[]);
};

#endif