## Step 1 — 新建 src/utils/metrics.h（MetricsCollector 单例）
线程安全的指标采集器，所有字段用 std::atomic<T>
字段：total_connections, active_connections, total_requests, total_recv_bytes, total_sent_bytes, start_time
方法：record_accept(), record_close(), record_request(), record_recv(n), record_sent(n)
snapshot() 返回结构体，含计算好的 requests_per_sec, throughput_kbps, uptime_seconds

## Step 2 — webserver.cpp 埋点（~15 行）
在 io_uring CQE 处理 + 线程池回调处调用 MetricsCollector：

accept 成功 → record_accept()
recv 完成 → record_recv(res)
write 完成 → record_sent(bytes_sent)
close 完成 → record_close()
请求处理完 → record_request()

## Step 3 — http_conn.cpp 添加 API 路由（~40 行）
在 do_request() 开头检测 /api/：

GET /api/stats → 调用 snapshot()，snprintf 生成 JSON，Content-Type: application/json，直接返回
GET /api/info → 返回编译信息（IO_URING 宏值、__DATE__、编译器版本）

## Step 4 — 新建 src/root/dashboard.html（~400 行）
纯 HTML/CSS/JS，零外部依赖，深色主题：

区域	内容
顶部横幅	服务器名 + io_uring/epoll 模式徽章
4 张指标卡	活跃连接 / 总请求 / req/s / 吞吐量 MB/s
实时折线图	<canvas> 绘制最近 60 秒请求速率（纯 JS）
技术栈卡片	9 张卡片展示每项技术 + 启用状态
架构流程图	静态 CSS/SVG 请求-响应链路
刷新机制	fetch('/api/stats') 每秒轮询

## Step 5 — 修改 judge.html（~3 行）
在已有菜单中添加入口按钮跳转 dashboard.html。

---

## 修改文件清单
文件	改动	说明
src/utils/metrics.h	新建 ~80 行	指标单例
webserver.cpp	~15 行	I/O 埋点
http_conn.cpp	~40 行	/api/stats + /api/info
src/root/dashboard.html	新建 ~400 行	仪表盘
judge.html	~3 行	入口链接