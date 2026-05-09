Plan: 子模块监控页面扩展
新增 5 个 API + 5 个子页面，通过 Dashboard 按钮跳转。

新增 API
端点	数据来源	返回
GET /api/mysql-pool	connection_pool 新增 GetStats()	活跃/空闲/最大连接
GET /api/thread-pool	threadpool 新增 GetStats()	线程数/队列长/最大队列/模型
GET /api/timer	MetricsCollector 新增字段	活跃定时器/累计超时
GET /api/log	Log 新增 GetInfo()	异步状态/文件路径
GET /api/config	MetricsCollector 新增字段	PORT/模式/线程数等

---

## Steps
### Step 1 — 扩展现有类（数据采集层）
文件	改动
CGImysql/sql_connection_pool.h	新增 struct PoolStats { int cur, free, max; } + PoolStats GetStats()
threadpool/threadpool.h	新增 struct TPoolStats { int threads, queue_size, max_queue, model; } + TPoolStats GetStats()
log/log.h	新增 bool is_async()、const char* log_path()、bool is_open()
timer/lst_timer.h	sort_timer_lst 新增 int size() 方法
utils/metrics.h	新增 record_timer_add/remove/expire + set_config(...) + 对应 atomic 字段

### Step 2 — http_conn.cpp 新增 5 个 API 路由
在 do_request() 中添加：

/api/mysql-pool → connection_pool::GetInstance()->GetStats() → JSON
/api/thread-pool → 通过全局/静态指针获取 threadpool stats → JSON
/api/timer → MetricsCollector::snapshot() → JSON
/api/log → Log::get_instance()->GetInfo() → JSON
/api/config → MetricsCollector 新增 ConfigSnapshot → JSON

### Step 3 — 埋点扩展
位置	调用
webserver.cpp init()	MetricsCollector::set_config(port, trigmode, ...)
timer() 中 add_timer 后	MetricsCollector::getInstance().record_timer_add()
cb_func() 中	MetricsCollector::getInstance().record_timer_expire()
sort_timer_lst::del_timer	MetricsCollector::getInstance().record_timer_remove()

### Step 4 — 创建 5 个子页面
文件	样式	内容
root/mysql.html	深色主题卡片	3 个指标卡（活跃/空闲/最大）+ 连接池利用率进度条
root/threadpool.html	同上	线程数 + 队列堆积 + Proactor/Reactor 模式
root/timer.html	同上	活跃定时器计数 + 超时累计
root/log.html	同上	异步/同步模式 + 日志路径 + 行数切分
root/config.html	同上	全部参数表格（PORT/模式/线程/SQL/linger 等）
每个页面有「返回 Dashboard」链接。

### Step 5 — 更新 dashboard.html
在技术栈卡片下方添加 5 个导航按钮，链接到各子页面。

---
完整改动文件清单
文件	改动量	说明
sql_connection_pool.h + .cpp	~15 行	PoolStats + GetStats()
threadpool.h	~10 行	TPoolStats + GetStats()
log/log.h + .cpp	~10 行	GetInfo()
timer/lst_timer.h + .cpp	~8 行	size()
utils/metrics.h	~30 行	timer 计数 + config
http/http_conn.cpp	~80 行	5 个 API 路由
webserver.cpp	~10 行	config 埋点 + timer 埋点
root/mysql.html	新建 ~180 行	MySQL 看板
root/threadpool.html	新建 ~180 行	线程池看板
root/timer.html	新建 ~150 行	定时器看板
root/log.html	新建 ~150 行	日志看板
root/config.html	新建 ~200 行	配置参数
root/dashboard.html	~15 行	导航按钮