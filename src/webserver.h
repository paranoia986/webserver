#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#if IO_URING
#include <liburing.h>
#endif

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"
#include "utils/path_util.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    //基础
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;

    int m_pipefd[2];

#if IO_URING
    struct io_uring ring;               // io_uring 实例（替换 epollfd + events）
#else
    int m_epollfd;
#endif

    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

#if !IO_URING
    //epoll_event相关（仅 epoll 模式需要）
    epoll_event events[MAX_EVENT_NUMBER];
#endif

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器相关
    client_data *users_timer;
    Utils utils;

#if IO_URING
private:
    // ── io_uring 辅助方法 ──
    void submit_accept_sqe();
    void submit_recv_sqe(int fd);
    void submit_writev_sqe(int fd);
    void submit_close_sqe(int fd);
    void submit_timeout_sqe();
    void submit_signal_recv_sqe();
    void handle_write_completion(int fd, int bytes_sent);
    void handle_signal_completion(int sig_count, bool &stop_server);

    char m_signal_buf[1024];     // 信号管道 io_uring recv 缓冲区
#endif

public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclientdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);
};
#endif
