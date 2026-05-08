顺序	文件	                     工作内容
1	CMakeLists.txt	            添加 option(IO_URING) 和add_compile_definitions  (1)

2	webserver.h	                条件成员 + ConnOp 枚举（1）

3	http_conn.h	                条件 ring 指针

4	lst_timer.h	                条件 ring 指针

5	webserver.cpp	            eventListen/eventLoop 重写

6	http_conn.cpp	            I/O 函数适配

7	lst_timer.cpp	            定时器适配

Steps
## Phase 1: 构建系统

### Step 1 — CMakeLists.txt

添加 option(IO_URING "Use io_uring" ON)，默认开启
if(IO_URING) 设置 add_compile_definitions(IO_URING=1) 并 find_package(liburing REQUIRED)
else() 设置 add_compile_definitions(IO_URING=0)
条件链接 target_link_libraries

---
## Phase 2: 头文件

### Step 2 — webserver.h（依赖 Step 1）

#if IO_URING：用 struct io_uring ring 替换 int m_epollfd 和 epoll_event events[]
新增连接操作状态枚举 ConnOp（ACCEPT/RECV/WRITE/CLOSE/TIMEOUT/SIGNAL）
保留 m_pipefd[2]（SIGINT/SIGTERM 仍需管道）

### Step 3 — http_conn.h（依赖 Step 1）

static int m_epollfd → 条件为 #if IO_URING 用 static struct io_uring *ring 指针
新增 int m_uring_state 成员追踪当前 pending 操作

### Step 4 — lst_timer.h（依赖 Step 1）

Utils 类中 static int u_epollfd → 条件为 static struct io_uring *u_ring

---
## Phase 3: 核心实现（最大改动）

### Step 5 — webserver.cpp（依赖 Step 2-4）

构造函数：#if IO_URING 下不创建 epollfd
析构函数：添加 io_uring_queue_exit(&ring)
eventListen() 重写：io_uring_queue_init(256) 替换 epoll_create，不调用 addfd()，改为提交初始 SQEs（accept + signal_recv + timeout）
eventLoop() 重写：io_uring_submit_and_wait 替换 epoll_wait，io_uring_for_each_cqe 遍历，用 cqe→user_data 编码分派到对应处理逻辑
dealclientdata() / dealwithread() / dealwithwrite()：#if !IO_URING 包裹

### Step 6 — http_conn.cpp（依赖 Step 3, 5）

addfd() / removefd() / modfd()：#if !IO_URING 包裹
read_once()：io_uring 模式改为 read_done(int bytes) —— recv 已由内核完成，直接取 m_read_buf 即可
write()：io_uring 模式改为检查 CQE 的 res 字段更新发送进度，残留数据重新提交 writev SQE
close_conn()：io_uring 模式用 io_uring_prep_close() 提交关闭

### Step 7 — lst_timer.cpp（依赖 Step 4, 5）

Utils::addfd()：#if !IO_URING 包裹
cb_func()：epoll 路径的 epoll_ctl(EPOLL_CTL_DEL) 条件化
timer_handler()：io_uring 模式移除 alarm() 调用

---
## Phase 4: 入口

### Step 8 — main.cpp — 无改动，入口不变