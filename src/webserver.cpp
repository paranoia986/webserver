#include "webserver.h"
#include <unistd.h>
#include <limits.h>
#include <string>
#if IO_URING == 1
    #include <liburing.h>
#endif

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    std::string base_dir = get_executable_path();
    
    // 拼接旁边的 root 文件夹 (注意：经过 CMake 同步后，root 就在可执行文件旁边)
    std::string root_dir = base_dir + "/root";

    // 转换为你的 C 风格字符串并分配内存
    m_root = (char *)malloc(root_dir.length() + 1);
    strcpy(m_root, root_dir.c_str());

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
#if IO_URING
    io_uring_queue_exit(&ring);
#else
    close(m_epollfd);
#endif
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
    free(m_root);
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("/run/mysqld/mysqld.sock", m_user, m_passWord, m_databaseName, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

#if IO_URING
    // ── io_uring 初始化 ──
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    ret = io_uring_queue_init_params(256, &ring, &params);
    assert(ret >= 0);

    // 设置全局 ring 指针（http_conn / Utils 均通过静态指针访问）
    http_conn::ring = &ring;
    Utils::u_ring   = &ring;

    // 信号管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.setnonblocking(m_pipefd[0]);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);
    utils.addsig(SIGINT,  utils.sig_handler, false);

    // 提交 3 个初始 SQE（accept / 信号管道 / 定时器）
    submit_accept_sqe();
    submit_signal_recv_sqe();
    submit_timeout_sqe();

    Utils::u_pipefd = m_pipefd;

#else
    // ── epoll 路径（原有代码）──
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);
    utils.addsig(SIGINT,  utils.sig_handler, false);

    alarm(TIMESLOT);

    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
#endif
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

#if !IO_URING
bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}
#endif // !IO_URING

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGINT:
            case SIGTERM:
            {
                alarm(0); // 取消闹钟，防止在确认期间定时器失效

                printf("\n\033\n[1;33m[Confirm] Are you sure you want to shut down the server? (y/n): \033[0m");
                fflush(stdout); // 确保提示语立即显示在终端

                char choice;
                // 读取用户输入，注意前面的空格是为了跳过换行符
                if (scanf(" %c", &choice) == 1 && (choice == 'y' || choice == 'Y')) 
                {
                    stop_server = true;
                    printf("\033\n[1;32m[System] Shutting down gracefully...\033[0m\n");
                } 
                else 
                {
                    stop_server = false;
                    printf("\033\n[1;36m[System] Shutdown cancelled. Server continues running.\033[0m\n");
                    // 重新开启闹钟，防止在确认期间定时器失效
                    alarm(TIMESLOT); 
                }
                break;
            }
            }
        }
    }
    return true;
}

#if !IO_URING
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
#endif // !IO_URING

#if !IO_URING
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
#endif // !IO_URING

void WebServer::eventLoop()
{
#if IO_URING
    // ============================================================
    //  io_uring 事件循环
    //  所有 I/O (accept/recv/writev/close/timeout) 由内核异步完成
    //  应用层通过 CQE 收割结果并驱动业务逻辑
    // ============================================================
    printf("    \n\033[1;32m[System] Server started on port %d with io_uring support!\033[0m\n", m_port);
    bool stop_server = false;

    // 提交 eventListen 中攒下的初始 SQE
    io_uring_submit(&ring);

    while (!stop_server)
    {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
        {
            if (ret == -EINTR)
                continue;              // 信号中断，下一轮继续
            LOG_ERROR("io_uring_wait_cqe: %s", strerror(-ret));
            break;
        }

        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring, head, cqe)
        {
            int     fd  = get_fd_from_user_data(cqe->user_data);
            ConnOp  op  = get_op_from_user_data(cqe->user_data);
            int     res = cqe->res;                // 操作返回值

            switch (op)
            {
            // ── Accept 完成 ──
            case OP_ACCEPT:
            {
                if (res >= 0)
                {
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        close(res);
                    }
                    else
                    {
                        struct sockaddr_in client_addr;
                        socklen_t addr_len = sizeof(client_addr);
                        getpeername(res, (struct sockaddr *)&client_addr, &addr_len);
                        timer(res, client_addr);

                        // 连接就绪 → 提交 recv SQE 开始读
                        submit_recv_sqe(res);
                    }
                }
                // 无论成功与否，重新提交 accept
                submit_accept_sqe();
                break;
            }

            // ── Recv 完成 ──
            case OP_RECV:
            {
                if (res > 0)
                {
                    // 数据已由内核写入 m_read_buf
                    users[fd].m_read_idx = res;
                    users[fd].m_uring_state = 0;

                    util_timer *timer = users_timer[fd].timer;
                    if (timer) adjust_timer(timer);

                    // 提交给线程池解析 HTTP
                    m_pool->append_p(users + fd);
                }
                else
                {
                    // 对端关闭或出错
                    submit_close_sqe(fd);
                }
                break;
            }

            // ── Write 完成 ──
            case OP_WRITE:
            {
                users[fd].m_uring_state = 0;
                handle_write_completion(fd, res);
                break;
            }

            // ── Close 完成 ──
            case OP_CLOSE:
            {
                users[fd].m_uring_state = 0;
                util_timer *timer = users_timer[fd].timer;
                if (timer)
                {
                    utils.m_timer_lst.del_timer(timer);
                    users_timer[fd].timer = nullptr;
                }
                if (users[fd].m_sockfd != -1)
                {
                    users[fd].m_sockfd = -1;
                    http_conn::m_user_count--;
                }
                break;
            }

            // ── Timeout 完成 ──
            case OP_TIMEOUT:
            {
                if (res == -ECANCELED) { submit_timeout_sqe(); break; }
                utils.timer_handler();
                LOG_INFO("%s", "timer tick");
                submit_timeout_sqe();
                break;
            }

            // ── Signal 管道数据到达 ──
            case OP_SIGNAL:
            {
                handle_signal_completion(res, stop_server);
                submit_signal_recv_sqe();
                break;
            }

            default:
                break;
            }
            count++;
        }
        io_uring_cq_advance(&ring, count);
    }

#else
    // ============================================================
    //  epoll 事件循环
    // ============================================================
    printf("    \n\033[1;32m[System] Server started on port %d with epoll support!\033[0m\n", m_port);
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag) continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
#endif
}

// ═══════════════════════════════════════════════════════════════
//  io_uring 辅助方法（仅在 IO_URING=1 时编译）
// ═══════════════════════════════════════════════════════════════
#if IO_URING

void WebServer::submit_accept_sqe()
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_accept(sqe, m_listenfd, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, make_user_data(m_listenfd, OP_ACCEPT));
}

void WebServer::submit_recv_sqe(int fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, fd, users[fd].m_read_buf, http_conn::READ_BUFFER_SIZE, 0);
    io_uring_sqe_set_data64(sqe, make_user_data(fd, OP_RECV));
    users[fd].m_uring_state = OP_RECV;
    io_uring_submit(&ring);
}

void WebServer::submit_writev_sqe(int fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_writev(sqe, fd, users[fd].m_iv, users[fd].m_iv_count, 0);
    io_uring_sqe_set_data64(sqe, make_user_data(fd, OP_WRITE));
    users[fd].m_uring_state = OP_WRITE;
    io_uring_submit(&ring);
}

void WebServer::submit_close_sqe(int fd)
{
    // 先 unmapping 文件映射
    users[fd].unmap();

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe)
    {
        // ring 满了，直接同步关闭
        util_timer *t = users_timer[fd].timer;
        if (t) { utils.m_timer_lst.del_timer(t); users_timer[fd].timer = nullptr; }
        if (users[fd].m_sockfd != -1) { close(users[fd].m_sockfd); users[fd].m_sockfd = -1; http_conn::m_user_count--; }
        return;
    }
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data64(sqe, make_user_data(fd, OP_CLOSE));
    users[fd].m_uring_state = OP_CLOSE;
    io_uring_submit(&ring);
}

void WebServer::submit_timeout_sqe()
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    struct __kernel_timespec ts = { .tv_sec = TIMESLOT, .tv_nsec = 0 };
    io_uring_prep_timeout(sqe, &ts, 0, IORING_TIMEOUT_ABS);
    io_uring_sqe_set_data64(sqe, make_user_data(0, OP_TIMEOUT));
}

void WebServer::submit_signal_recv_sqe()
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, m_pipefd[0], m_signal_buf, sizeof(m_signal_buf), 0);
    io_uring_sqe_set_data64(sqe, make_user_data(m_pipefd[0], OP_SIGNAL));
}

void WebServer::handle_write_completion(int fd, int bytes_sent)
{
    if (bytes_sent <= 0)
    {
        submit_close_sqe(fd);
        return;
    }

    http_conn &conn = users[fd];
    conn.bytes_have_send += bytes_sent;
    conn.bytes_to_send   -= bytes_sent;

    if (conn.bytes_to_send <= 0)
    {
        // 全部发送完成
        conn.unmap();
        if (conn.m_linger)
        {
            // keep-alive：重置连接状态，提交下一个 recv
            conn.init();
            submit_recv_sqe(fd);
        }
        else
        {
            submit_close_sqe(fd);
        }
    }
    else
    {
        // 还有剩余数据，更新 iovec 后重新提交 writev
        if (conn.bytes_have_send >= conn.m_iv[0].iov_len)
        {
            conn.m_iv[0].iov_len = 0;
            conn.m_iv[1].iov_base = conn.m_file_address + (conn.bytes_have_send - conn.m_write_idx);
            conn.m_iv[1].iov_len = conn.bytes_to_send;
        }
        else
        {
            conn.m_iv[0].iov_base = conn.m_write_buf + conn.bytes_have_send;
            conn.m_iv[0].iov_len = conn.m_iv[0].iov_len - bytes_sent;
        }
        submit_writev_sqe(fd);
    }
}

void WebServer::handle_signal_completion(int sig_count, bool &stop_server)
{
    if (sig_count <= 0) return;

    for (int i = 0; i < sig_count; ++i)
    {
        switch (m_signal_buf[i])
        {
        case SIGALRM:
            // timeout 由 io_uring timeout SQE 处理，此处忽略
            break;
        case SIGINT:
        case SIGTERM:
        {
            printf("\n\033[1;33m[Confirm] Are you sure you want to shut down the server? (y/n): \033[0m");
            fflush(stdout);
            char choice;
            if (scanf(" %c", &choice) == 1 && (choice == 'y' || choice == 'Y'))
            {
                stop_server = true;
                printf("\033[1;32m[System] Shutting down gracefully...\033[0m\n");
            }
            else
            {
                stop_server = false;
                printf("\033[1;36m[System] Shutdown cancelled. Server continues running.\033[0m\n");
            }
            break;
        }
        default:
            break;
        }
    }
}

#endif // IO_URING
