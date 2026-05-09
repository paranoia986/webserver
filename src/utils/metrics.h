#ifndef METRICS_H
#define METRICS_H

#include <atomic>
#include <ctime>
#include <cstdint>
#include <mutex>

// ── 配置快照 ──
struct ConfigSnapshot
{
    int port;
    int io_uring;
    int trig_mode;
    int listen_trigmode;
    int conn_trigmode;
    int opt_linger;
    int sql_num;
    int thread_num;
    int close_log;
    int actor_model;
};

// ── 指标快照 ──
struct MetricsSnapshot
{
    uint64_t total_connections;
    int      active_connections;
    uint64_t total_requests;
    uint64_t total_recv_bytes;
    uint64_t total_sent_bytes;

    int      active_timers;
    uint64_t expired_timers;

    uint64_t uptime_seconds;

    double   requests_per_sec;
    double   recv_kbps;
    double   sent_kbps;

    int      io_uring_enabled;
    int      thread_pool_size;
    int      mysql_pool_size;

    int      tpool_queue_size;
    int      tpool_model;

    ConfigSnapshot config;
};

class MetricsCollector
{
public:
    static MetricsCollector &getInstance()
    {
        static MetricsCollector instance;
        return instance;
    }

    void record_accept()
    {
        active_connections_.fetch_add(1, std::memory_order_relaxed);
        total_connections_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_close()
    {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }
    void record_request()
    {
        total_requests_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_recv(uint64_t n)
    {
        total_recv_bytes_.fetch_add(n, std::memory_order_relaxed);
    }
    void record_sent(uint64_t n)
    {
        total_sent_bytes_.fetch_add(n, std::memory_order_relaxed);
    }

    void record_timer_add()    { active_timers_.fetch_add(1, std::memory_order_relaxed); }
    void record_timer_remove() { active_timers_.fetch_sub(1, std::memory_order_relaxed); }
    void record_timer_expire()
    {
        expired_timers_.fetch_add(1, std::memory_order_relaxed);
    }

    void set_thread_pool_size(int n) { thread_pool_size_.store(n); }
    void set_mysql_pool_size(int n)  { mysql_pool_size_.store(n); }
    void set_tpool_status(int queue, int model) { tpool_queue_.store(queue); tpool_model_.store(model); }
    void set_config(const ConfigSnapshot &c) { config_ = c; }

    ConfigSnapshot get_config() const { return config_; }

    MetricsSnapshot snapshot()
    {
        MetricsSnapshot s;
        s.total_connections  = total_connections_.load(std::memory_order_relaxed);
        s.active_connections = active_connections_.load(std::memory_order_relaxed);
        s.total_requests     = total_requests_.load(std::memory_order_relaxed);
        s.total_recv_bytes   = total_recv_bytes_.load(std::memory_order_relaxed);
        s.total_sent_bytes   = total_sent_bytes_.load(std::memory_order_relaxed);
        s.active_timers      = active_timers_.load(std::memory_order_relaxed);
        s.expired_timers     = expired_timers_.load(std::memory_order_relaxed);
        s.uptime_seconds     = static_cast<uint64_t>(time(nullptr) - start_time_);

        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            time_t now = time(nullptr);
            double dt  = difftime(now, last_time_);
            if (dt > 0.0)
            {
                s.requests_per_sec = static_cast<double>(s.total_requests - last_requests_) / dt;
                s.recv_kbps        = static_cast<double>(s.total_recv_bytes - last_recv_) / dt / 1024.0;
                s.sent_kbps        = static_cast<double>(s.total_sent_bytes - last_sent_) / dt / 1024.0;
            }
            else
            {
                s.requests_per_sec = 0.0; s.recv_kbps = 0.0; s.sent_kbps = 0.0;
            }
            last_requests_ = s.total_requests;
            last_recv_     = s.total_recv_bytes;
            last_sent_     = s.total_sent_bytes;
            last_time_     = now;
        }

#if IO_URING
        s.io_uring_enabled = 1;
#else
        s.io_uring_enabled = 0;
#endif
        s.thread_pool_size = thread_pool_size_.load();
        s.mysql_pool_size  = mysql_pool_size_.load();
        s.tpool_queue_size = tpool_queue_.load();
        s.tpool_model      = tpool_model_.load();
        s.config           = config_;
        return s;
    }

private:
    MetricsCollector() : start_time_(time(nullptr))
    {
        total_connections_.store(0, std::memory_order_relaxed);
        active_connections_.store(0, std::memory_order_relaxed);
        total_requests_.store(0, std::memory_order_relaxed);
        total_recv_bytes_.store(0, std::memory_order_relaxed);
        total_sent_bytes_.store(0, std::memory_order_relaxed);
        active_timers_.store(0, std::memory_order_relaxed);
        expired_timers_.store(0, std::memory_order_relaxed);
        last_requests_ = 0; last_recv_ = 0; last_sent_ = 0;
        last_time_ = start_time_;
    }
    MetricsCollector(const MetricsCollector &) = delete;
    MetricsCollector &operator=(const MetricsCollector &) = delete;

    std::atomic<uint64_t> total_connections_;
    std::atomic<int>      active_connections_;
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> total_recv_bytes_;
    std::atomic<uint64_t> total_sent_bytes_;
    std::atomic<int>      active_timers_;
    std::atomic<uint64_t> expired_timers_;
    time_t                start_time_;

    std::atomic<int>      thread_pool_size_{-1};
    std::atomic<int>      mysql_pool_size_{-1};
    std::atomic<int>      tpool_queue_{0};
    std::atomic<int>      tpool_model_{0};
    ConfigSnapshot        config_;

    std::mutex snap_mutex_;
    uint64_t   last_requests_;
    uint64_t   last_recv_;
    uint64_t   last_sent_;
    time_t     last_time_;
};

#endif // METRICS_H
