# ⚡ WebServer — 高性能 C++ HTTP 服务器

基于 C++17 构建的高性能 HTTP/1.1 服务器，支持 **io_uring 异步 I/O** 与 **epoll 多路复用**双模式切换，集成 MySQL 连接池、线程池调度、异步日志、HTTP Range（206 Partial Content）、实时监控仪表盘及 Watchdog 文件自动同步 CDN。

---

## 功能模块

### 1. HTTP/1.1 解析器

- 有限状态机（请求行 → 头部 → 内容体）逐步解析
- 支持 `GET` / `POST` 方法，`keep-alive` 长连接
- CGI 动态路由：登录/注册/图片/视频全流程

### 2. io_uring 异步 I/O（Linux 5.1+）

- 通过 SQE/CQE 环形队列实现 **零系统调用开销** 的 `accept` / `recv` / `writev` / `close`
- 编译宏 `IO_URING` 双模式切换：epoll（LT/ET + EPOLLONESHOT）⇄ io_uring
- 定时器用 `IORING_OP_TIMEOUT` 替代 `SIGALRM + alarm()`
- 用户数据编码：`cqe->user_data = (fd << 32) | ConnOp`

### 3. 线程池（Proactor / Reactor 双模型）

- pthread 半同步/半反应器，信号量 + 互斥锁生产者-消费者模型
- `actor_model=0` → Proactor / `actor_model=1` → Reactor

### 4. MySQL 连接池

- RAII 封装（`connectionRAII`），Unix Domain Socket / TCP 双模式
- `PoolStats GetStats()` 实时监控连接池状态

### 5. 升序双向链表定时器

- 每连接独立定时器（15s 超时），数据到达自动续期，超时自动清理

### 6. mmap + writev 零拷贝 + HTTP Range

- `mmap()` 文件映射 + `writev()` 聚集写，避免用户态拷贝
- **HTTP 206 Partial Content**：`Content-Range` + `Accept-Ranges: bytes`
- 浏览器可拖拽进度条 seek 视频

### 7. 异步日志系统

- 单例 + 阻塞队列 + 后端线程，按天/按行数翻滚，ANSI 颜色码区分级别

### 8. YAML 配置管理

- `application.yaml` 集中管理所有参数，命令行覆盖

---

## 工程能力

### 构建系统

- Conan + CMake，`-DIO_URING=ON/OFF` 一键切换 io_uring / epoll

### 内存安全

```
definitely lost: 0 bytes     ← Valgrind Memcheck 验证
indirectly lost: 0 bytes
still reachable: 0 bytes     ← mysql_library_end() 清理
possibly lost:  2,560 bytes  ← glibc pthread TLS（已知行为，可忽略）
```

### 实时监控仪表盘（纯 HTML/CSS/JS，零依赖）

| 端点                                            | 内容                                                  |
| ----------------------------------------------- | ----------------------------------------------------- |
| `dashboard.html`                              | 指标卡 + Canvas 折线图 + 技术栈 + 架构图 + 5 个子模块 |
| `/api/stats`                                  | 连接/请求/速率/吞吐量/定时器/配置 JSON                |
| `/api/mysql-pool`                             | MySQL 连接池：活跃/空闲/最大 + 利用率                 |
| `/api/thread-pool`                            | 线程池：线程数/队列/模型                              |
| `/api/timer` / `/api/log` / `/api/config` | 定时器/日志/全部参数                                  |

### Watchdog 文件同步 "微型 CDN"

- Python + `watchdog` 监听 `dropzone/`，文件放入即同步到 root/ 并生成 `index.html`
- 图片自动缩略图，视频内嵌 `<video>` 播放器（依赖 HTTP Range）

### 前端美化

- 全站深色 GitHub 风格主题，8 个页面统一 UI 完整导航链路

---

## 量化结果

| 指标       | 数值                   | 验证                        |
| ---------- | ---------------------- | --------------------------- |
| 内存泄漏   | **0 bytes**      | Valgrind                    |
| HTTP Range | ✅ 206 Partial Content | 浏览器 `<video>` seek     |
| 编译双模式 | ✅ io_uring / epoll    | `-DIO_URING=ON/OFF`       |
| 监控指标   | ✅ 11 个实时指标       | `dashboard.html` 每秒刷新 |
| CDN 同步   | ✅ 投放即展示          | watchdog 守护进程           |

---

## 技术栈

| 层级   | 技术                                   |
| ------ | -------------------------------------- |
| I/O    | io_uring / epoll                       |
| 并发   | pthread 线程池（Proactor/Reactor）     |
| 传输   | mmap + writev（零拷贝）                |
| HTTP   | 有限状态机，HTTP/1.1 + Range           |
| 数据库 | MySQL + RAII 连接池                    |
| 定时器 | 升序双向链表                           |
| 日志   | 单例异步（按天/按行）                  |
| 配置   | yaml-cpp                               |
| 构建   | Conan + CMake                          |
| 监控   | 纯 HTML/CSS/JS Dashboard + RESTful API |
| 同步   | Python watchdog CDN                    |
| 内存   | Valgrind 零泄漏                        |

---

## 架构概览

```text
                          ┌──────────────────────┐
                          │    Browser / Client   │
                          └────────┬─────────────┘
                                   │ HTTP Request
                                   ▼
                          ┌──────────────────────┐
                          │    io_uring Ring      │
                          │  accept/recv/writev   │
                          └────────┬─────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    ▼              ▼              ▼
              Read Buffer    Thread Pool     MySQL Pool
              (mmap+writev)  (pthread×N)    (RAII)
                    │              │              │
                    ▼              ▼              ▼
              HTTP Parser ──► CGI Router ──► Response
                    │
                    ▼
            ⏱️ Timer Heap  📝 Async Logger  📊 MetricsCollector
                                   │
                                   ▼
                          🌐 Dashboard / API
                          watchdog CDN sync
```

---

## 快速开始

### 依赖

- Linux 5.1+（io_uring）或任意 Linux（epoll 回退）
- GCC 13+ / Clang 15+
- Conan 2.x / CMake 3.15+
- MySQL 8.0+（需运行实例）
- Python 3.8+ + `pip install watchdog`（CDN 同步）

### 构建

```bash
git clone https://github.com/paranoia986/webserver.git
cd webserver

# io_uring 模式（默认）
conan install . -o io_uring=True -s build_type=Debug --build=missing
conan build . -s build_type=Debug

# epoll 回退模式
conan install . -o io_uring=False -s build_type=Debug --build=missing
conan build . -s build_type=Debug
```

### 运行

```bash
./build/Debug/server                        # 启动服务器
python ./file_sync.py &                     # 启动 CDN 守护进程
# 浏览器访问 http://127.0.0.1:9006
```

### 内存检查

```bash
valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes --log-file=valgrind_report.txt \
         ./build/Debug/server
```

---

## 文件结构

```text
webserver/
├── CMakeLists.txt
├── conanfile.py
├── file_sync.py                # CDN 看守进程
├── dropzone/                   # CDN 投放区
├── src/
│   ├── webserver.cpp/h         # 核心事件循环
│   ├── http/http_conn.cpp/h    # HTTP 解析 + Range + API
│   ├── threadpool/threadpool.h
│   ├── CGImysql/sql_connection_pool.cpp/h
│   ├── timer/lst_timer.cpp/h
│   ├── log/log.cpp/h + block_queue.h
│   ├── lock/locker.h
│   ├── utils/metrics.h + path_util.h
│   ├── YAMLparser/parser.cpp/h
│   └── root/                   # 静态资源 + Dashboard
└── test_pressure/webbench-1.5/
```
