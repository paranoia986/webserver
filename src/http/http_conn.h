#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#if IO_URING == 1
    #include <liburing.h>
#endif

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

// io_uring 操作类型枚举 —— 编码在 cqe->user_data 低 32 位
// （定义在 http_conn.h 以便 webserver.cpp 和 http_conn.cpp 共享）
enum ConnOp : unsigned {
    OP_ACCEPT  = 0,   // 接受新连接
    OP_RECV    = 1,   // 读取客户端数据
    OP_WRITE   = 2,   // 发送响应数据
    OP_CLOSE   = 3,   // 关闭连接
    OP_TIMEOUT = 4,   // 定时器超时
    OP_SIGNAL  = 5,   // 信号管道可读
};

// 辅助函数：打包/拆解 cqe->user_data
inline unsigned long long make_user_data(int fd, ConnOp op) {
    return ((unsigned long long)fd << 32) | (unsigned long long)op;
}
inline int  get_fd_from_user_data(unsigned long long data) { return (int)(data >> 32); }
inline ConnOp get_op_from_user_data(unsigned long long data) { return (ConnOp)(data & 0xFFFFFFFF); }

class http_conn
{
    friend class WebServer;  // io_uring 模式下 WebServer 需要访问私有成员
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
        API_RESPONSE       // /api/* 路由直接返回
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
#if IO_URING
    static struct io_uring *ring;         // 指向 WebServer 的 io_uring 实例
#else
    static int m_epollfd;
#endif
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
#if IO_URING
    int m_uring_state;   // 当前 pending 的 io_uring 操作 (ConnOp), 0=空闲
#endif
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
