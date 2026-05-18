#include "http_conn.h"
#include "utils/metrics.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }

    mysql_free_result(result);
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

#if !IO_URING
//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
#endif // !IO_URING

int http_conn::m_user_count = 0;
#if IO_URING
struct io_uring *http_conn::ring = nullptr;
#else
int http_conn::m_epollfd = -1;
#endif

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (!real_close || m_sockfd == -1) return;

#if IO_URING
    // io_uring 路径：提交异步 close SQE
    printf("close %d\n", m_sockfd);
    unmap();

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe)
    {
        io_uring_prep_close(sqe, m_sockfd);
        io_uring_sqe_set_data64(sqe, make_user_data(m_sockfd, OP_CLOSE));
        m_uring_state = OP_CLOSE;
        io_uring_submit(ring);
    }
    else
    {
        // ring 满，回退同步关闭
        close(m_sockfd);
        m_user_count--;
    }
    // 不在此处改 m_sockfd = -1，留给 CQE 处理
#else
    printf("close %d\n", m_sockfd);
    unmap();
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--;
#endif
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    m_TRIGMode = TRIGMode;

#if IO_URING
    m_uring_state = 0;
#else
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
#endif
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    m_range_start = 0;
    m_range_end   = -1;
    m_has_range   = false;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

#if !IO_URING
//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}
#endif // !IO_URING

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Range:", 6) == 0)
    {
        // 解析 Range: bytes=X-Y 或 bytes=X-
        text += 6;
        text += strspn(text, " \t");
        if (strncasecmp(text, "bytes=", 6) == 0)
        {
            text += 6;
            m_range_start = atol(text);
            char *dash = strchr(text, '-');
            if (dash && dash[1] != '\0')
                m_range_end = atol(dash + 1);
            else
                m_range_end = -1;   // 到文件末尾
            m_has_range = true;
        }
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // ── API 路由（优先级最高）──
    if (strncmp(m_url, "/api/", 5) == 0)
    {
        if (strncmp(m_url, "/api/stats", 10) == 0)
        {
            auto &mc = MetricsCollector::getInstance();
            auto s   = mc.snapshot();

            char json[1536];
            int json_len = snprintf(json, sizeof(json),
                "{"
                "\"active_connections\":%d,"
                "\"total_connections\":%llu,"
                "\"total_requests\":%llu,"
                "\"total_recv_mb\":%.2f,"
                "\"total_sent_mb\":%.2f,"
                "\"active_timers\":%d,"
                "\"expired_timers\":%llu,"
                "\"uptime_seconds\":%llu,"
                "\"requests_per_sec\":%.1f,"
                "\"recv_kbps\":%.1f,"
                "\"sent_kbps\":%.1f,"
                "\"io_uring\":%d,"
                "\"thread_pool_size\":%d,"
                "\"mysql_pool_size\":%d,"
                "\"tpool_queue_size\":%d,"
                "\"tpool_model\":%d"
                "}",
                s.active_connections,
                (unsigned long long)s.total_connections,
                (unsigned long long)s.total_requests,
                s.total_recv_bytes / 1048576.0,
                s.total_sent_bytes / 1048576.0,
                s.active_timers,
                (unsigned long long)s.expired_timers,
                (unsigned long long)s.uptime_seconds,
                s.requests_per_sec,
                s.recv_kbps,
                s.sent_kbps,
                s.io_uring_enabled,
                s.thread_pool_size,
                s.mysql_pool_size,
                s.tpool_queue_size,
                s.tpool_model
            );

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n"
                "\r\n"
                "%s",
                json_len, json
            );
            return API_RESPONSE;
        }

        if (strncmp(m_url, "/api/info", 9) == 0)
        {
            char json[512];
            int json_len = snprintf(json, sizeof(json),
                "{"
                "\"io_uring\":%d,"
                "\"compiler\":\"GCC %d.%d.%d\","
                "\"build_date\":\"%s\","
                "\"build_time\":\"%s\""
                "}",
#if IO_URING
                1,
#else
                0,
#endif
                __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
                __DATE__, __TIME__
            );

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n"
                "\r\n"
                "%s",
                json_len, json
            );
            return API_RESPONSE;
        }

        // ── 新增子模块 API ──

        if (strncmp(m_url, "/api/mysql-pool", 15) == 0)
        {
            auto *pool = connection_pool::GetInstance();
            auto s     = pool->GetStats();
            char json[256];
            int json_len = snprintf(json, sizeof(json),
                "{\"cur_conn\":%d,\"free_conn\":%d,\"max_conn\":%d,\"utilization\":%.1f}",
                s.cur_conn, s.free_conn, s.max_conn,
                s.max_conn > 0 ? (100.0 * s.cur_conn / s.max_conn) : 0.0);

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n%s", json_len, json);
            return API_RESPONSE;
        }

        if (strncmp(m_url, "/api/thread-pool", 16) == 0)
        {
            auto &mc = MetricsCollector::getInstance();
            auto s   = mc.snapshot();
            char json[256];
            int json_len = snprintf(json, sizeof(json),
                "{\"threads\":%d,\"queue_size\":%d,\"max_queue\":10000,\"model\":%d,\"model_name\":\"%s\"}",
                s.thread_pool_size, s.tpool_queue_size, s.tpool_model,
                s.tpool_model == 0 ? "proactor" : "reactor");

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n%s", json_len, json);
            return API_RESPONSE;
        }

        if (strncmp(m_url, "/api/timer", 10) == 0)
        {
            auto &mc = MetricsCollector::getInstance();
            auto s   = mc.snapshot();
            char json[128];
            int json_len = snprintf(json, sizeof(json),
                "{\"active_timers\":%d,\"expired_timers\":%llu}",
                s.active_timers, (unsigned long long)s.expired_timers);

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n%s", json_len, json);
            return API_RESPONSE;
        }

        if (strncmp(m_url, "/api/log", 8) == 0)
        {
            auto *log = Log::get_instance();
            char json[512];
            int json_len = snprintf(json, sizeof(json),
                "{\"is_async\":%d,\"is_open\":%d,\"split_lines\":%d,\"count\":%lld,\"dir\":\"%s\",\"file\":\"%s\"}",
                log->is_async() ? 1 : 0, log->is_open() ? 1 : 0,
                log->split_lines(), (long long)log->count(),
                log->log_dir(), log->log_file());

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n%s", json_len, json);
            return API_RESPONSE;
        }

        if (strncmp(m_url, "/api/config", 11) == 0)
        {
            auto &mc = MetricsCollector::getInstance();
            auto c   = mc.get_config();
            char json[512];
            int json_len = snprintf(json, sizeof(json),
                "{"
                "\"port\":%d,\"io_uring\":%d,\"trig_mode\":%d,"
                "\"listen_trigmode\":%d,\"conn_trigmode\":%d,"
                "\"opt_linger\":%d,\"sql_num\":%d,\"thread_num\":%d,"
                "\"close_log\":%d,\"actor_model\":%d"
                "}",
                c.port, c.io_uring, c.trig_mode,
                c.listen_trigmode, c.conn_trigmode,
                c.opt_linger, c.sql_num, c.thread_num,
                c.close_log, c.actor_model);

            m_write_idx = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Content-Length: %d\r\n\r\n%s", json_len, json);
            return API_RESPONSE;
        }

        // 未知 API 端点
        return BAD_REQUEST;
    }

    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
            free(sql_insert);
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
#if !IO_URING
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
#endif // !IO_URING
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    // 判断请求的文件后缀，返回正确的 Content-Type
    if (strstr(m_real_file, ".html")) {
        return add_response("Content-Type:%s\r\n", "text/html; charset=utf-8");
    } 
    else if (strstr(m_real_file, ".mp4")) {
        return add_response("Content-Type:%s\r\n", "video/mp4");
    } 
    else if (strstr(m_real_file, ".jpg") || strstr(m_real_file, ".jpeg")) {
        return add_response("Content-Type:%s\r\n", "image/jpeg");
    } 
    else if (strstr(m_real_file, ".png")) {
        return add_response("Content-Type:%s\r\n", "image/png");
    }
    else if (strstr(m_real_file, ".gif")) {
        return add_response("Content-Type:%s\r\n", "image/gif");
    }
    else if (strstr(m_real_file, ".css")) {
        return add_response("Content-Type:%s\r\n", "text/css");
    }
    else if (strstr(m_real_file, ".js")) {
        return add_response("Content-Type:%s\r\n", "application/javascript");
    }
    // 如果都不匹配，默认作为二进制流下载
    else {
        return add_response("Content-Type:%s\r\n", "application/octet-stream");
    }
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        if (m_has_range)
        {
            // ── HTTP 206 Partial Content ──
            long file_size   = m_file_stat.st_size;
            if (m_range_end == -1 || m_range_end >= file_size)
                m_range_end = file_size - 1;
            long content_len  = m_range_end - m_range_start + 1;

            int header_len = snprintf(m_write_buf, WRITE_BUFFER_SIZE - 1,
                "HTTP/1.1 206 Partial Content\r\n"
                "Content-Range: bytes %ld-%ld/%ld\r\n"
                "Accept-Ranges: bytes\r\n",
                m_range_start, m_range_end, file_size);
            m_write_idx = header_len;
            add_content_type();
            add_response("Content-Length: %ld\r\n", content_len);
            add_blank_line();

            // 响应头 → m_iv[0]，文件 Range 数据 → m_iv[1]
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len  = static_cast<size_t>(m_write_idx);
            m_iv[1].iov_base = m_file_address + m_range_start;
            m_iv[1].iov_len  = static_cast<size_t>(content_len);
            m_iv_count       = 2;
            bytes_to_send    = m_write_idx + content_len;
            return true;
        }

        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    case API_RESPONSE:
    {
        // API 响应已在 do_request() 中完整构建到 m_write_buf
        // 直接发送即可
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

#if IO_URING
    // ── io_uring 路径 ──
    if (read_ret == NO_REQUEST)
    {
        // 需要更多数据 → 重新提交 recv SQE
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (sqe)
        {
            io_uring_prep_recv(sqe, m_sockfd, m_read_buf + m_read_idx,
                               READ_BUFFER_SIZE - m_read_idx, 0);
            io_uring_sqe_set_data64(sqe, make_user_data(m_sockfd, OP_RECV));
            m_uring_state = OP_RECV;
            io_uring_submit(ring);
        }
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        // 响应构建失败 → 提交异步 close
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (sqe)
        {
            io_uring_prep_close(sqe, m_sockfd);
            io_uring_sqe_set_data64(sqe, make_user_data(m_sockfd, OP_CLOSE));
            m_uring_state = OP_CLOSE;
            io_uring_submit(ring);
        }
        return;
    }

    // 提交异步 writev，然后 io_uring 事件循环负责收割 CQE
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe)
    {
        MetricsCollector::getInstance().record_request();
        io_uring_prep_writev(sqe, m_sockfd, m_iv, m_iv_count, 0);
        io_uring_sqe_set_data64(sqe, make_user_data(m_sockfd, OP_WRITE));
        m_uring_state = OP_WRITE;
        io_uring_submit(ring);
    }

#else
    // ── epoll 路径（原有代码）──
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
#endif
}
