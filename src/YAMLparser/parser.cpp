#include "parser.h"
#include <iostream>
#include <cstdlib>

app_config::app_config(const std::string& filename){
    try {
        // 尝试加载和解析 YAML 文件
        YAML::Node config = YAML::LoadFile(filename);

        PORT = config["server"]["PORT"].as<int>();
        LOGWrite = config["server"]["LOGWrite"].as<int>();
        TRIGMode = config["server"]["TRIGMode"].as<int>();
        LISTENTrigmode = config["server"]["LISTENTrigmode"].as<int>();
        CONNTrigmode = config["server"]["CONNTrigmode"].as<int>();
        OPT_LINGER = config["server"]["OPT_LINGER"].as<int>();
        sql_num = config["server"]["sql_num"].as<int>();
        thread_num = config["server"]["thread_num"].as<int>();
        close_log = config["server"]["close_log"].as<int>();
        actor_model = config["server"]["actor_model"].as<int>();

        user = config["database"]["user"].as<std::string>();
        password = config["database"]["password"].as<std::string>();
        name = config["database"]["name"].as<std::string>();

    } catch (const YAML::Exception& e) {
        // 捕获到 yaml-cpp 抛出的异常
        std::cerr << "Fatal Error: 加载或解析配置文件失败 (" << filename << ")" << std::endl;
        std::cerr << "详细错误信息: " << e.what() << std::endl;
        
        // 立即退出程序，1 表示非正常退出
        exit(1);
    }
}

void app_config::parse_arg(int argc, char*argv[]){
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:u:w:n:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            PORT = atoi(optarg);
            break;
        }
        case 'l':
        {
            LOGWrite = atoi(optarg);
            break;
        }
        case 'm':
        {
            TRIGMode = atoi(optarg);
            break;
        }
        case 'o':
        {
            OPT_LINGER = atoi(optarg);
            break;
        }
        case 's':
        {
            sql_num = atoi(optarg);
            break;
        }
        case 't':
        {
            thread_num = atoi(optarg);
            break;
        }
        case 'c':
        {
            close_log = atoi(optarg);
            break;
        }
        case 'a':
        {
            actor_model = atoi(optarg);
            break;
        }
        case 'u':
        {
            user = optarg;
            break;
        }
        case 'w':
        {
            password = optarg;
            break;
        }
        case 'n':
        {
            name = optarg;
            break;
        }
        default:
            break;
        }
    }
}