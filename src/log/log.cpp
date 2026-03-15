#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
#include "../utils/path_util.h"
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
    m_close_log = 1; 
    m_fp = NULL;
    m_buf = NULL;
    m_log_queue = NULL;
}

Log::~Log()
{
    // 安全停掉后台的异步写入线程
    // 必须在释放队列内存之前做！
    if (m_is_async && m_log_queue != NULL)
    {
        // 告诉队列要关闭了，并叫醒正在等待的后台线程
        m_log_queue->close_queue();
        
        // 延时 1 毫秒 (1000 微秒)
        // 极其重要：给后台线程一点点时间，让它把 pop 函数里的 return false 走完，结束线程的 while 循环
        usleep(1000); 
    }

    // 关闭文件描述符
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }

    // 释放字符串缓冲区内存 (修复内存泄漏)
    if (m_buf != NULL)
    {
        delete[] m_buf;
        m_buf = NULL; // 释放后置空是 C++ 的好习惯
    }

    // 释放阻塞队列内存
    // 此时后台线程已经退出了，delete 它是绝对安全的
    if (m_log_queue != NULL)
    {
        delete m_log_queue;
        m_log_queue = NULL;
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 获取程序运行的绝对路径
    std::string base_path = get_executable_path();
    
    // 拼接专属的日志文件夹路径，注意末尾带上 '/'
    std::string log_dir = base_path + "/ServerLog/";
    
    // 自动创建该文件夹（0777为最高权限，若已存在则返回-1，不影响程序）
    mkdir(log_dir.c_str(), 0777);

    // 将目录名和文件名分别保存到 Log 类的成员变量中
    // (这是为了后续 write_log 按天翻滚生成新文件时，依然能找到这个目录)
    strncpy(dir_name, log_dir.c_str(), sizeof(dir_name) - 1);
    dir_name[sizeof(dir_name) - 1] = '\0';
    
    strncpy(log_name, file_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';

    // 拼接当天第一次打开的完整绝对路径日志文件名
    char log_full_name[512] = {0};
    snprintf(log_full_name, 511, "%s%d_%02d_%02d_%s", 
             dir_name, 
             my_tm.tm_year + 1900, 
             my_tm.tm_mon + 1, 
             my_tm.tm_mday, 
             log_name);

    m_today = my_tm.tm_mday;
    
    // 追加模式打开文件
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        // 增加 perror 方便调试：如果没打开，会在终端告诉你为什么（比如权限不够）
        perror("Log Init Failed: 无法打开日志文件");
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (level == 2 || level == 3) 
    {
        // 使用 ANSI 颜色码：Warn 为黄色 (\033[33m)，Error 为红色 (\033[31m)
        if (level == 2) 
            printf("\033[33m%s\033[0m", log_str.c_str());
        else if (level == 3)
            fprintf(stderr, "\033[31m%s\033[0m", log_str.c_str()); // Error 通常输出到 stderr
    }

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
