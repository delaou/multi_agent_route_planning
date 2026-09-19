#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// RedisCache 执行统计。计数器用于健康接口和压测分析，不参与调度决策。
struct RedisCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t writes = 0;
    uint64_t errors = 0;
    uint64_t bypasses = 0;
};

// Optional, fail-open Redis response cache. The implementation is compiled only
// when hiredis is available; otherwise init() returns false and every operation
// becomes a cheap cache miss so the server keeps its original in-memory path.
class RedisCache {
public:
    // 返回进程内唯一 Redis 客户端池，供 HTTP 缓存和实时数据层共享。
    static RedisCache& instance();

    // 建立最多 pool_size 个 hiredis 连接。只要至少一个连接成功即可启用；
    // 未编译 hiredis 或 Redis 不可达时返回 false，调用方可按降级策略继续运行。
    bool init(const std::string& host,
              int port,
              const std::string& password,
              int database,
              int pool_size,
              int connect_timeout_ms,
              int command_timeout_ms,
              int default_ttl_ms,
              const std::string& key_prefix);

    // 读取带统一前缀的 String；不存在、连接池繁忙或 Redis 故障均返回 nullopt。
    std::optional<std::string> get(const std::string& key);
    // 写入带毫秒 TTL 的 String；ttl_ms<=0 时使用初始化时的默认 TTL。
    bool set(const std::string& key,
             const std::string& value,
             int ttl_ms = 0);
    // 写入不过期的 String，适合显式版本指针；业务数据仍应优先设置 TTL。
    bool setPersistent(const std::string& key, const std::string& value);
    // 删除一个带统一前缀的 Key；Key 不存在也视为命令执行成功。
    bool remove(const std::string& key);
    // 使用一条 DEL 命令批量删除固定 Key，减少缓存失效的网络往返。
    void remove(std::initializer_list<const char*> keys);

    // 向 Redis Stream 追加一个事件。payload 通常为 JSON；MAXLEN 使用近似裁剪，
    // 避免遥测流无限增长。返回 Redis 生成的事件 ID，失败返回 nullopt。
    std::optional<std::string> appendStream(
        const std::string& stream,
        const std::string& event_type,
        const std::string& aggregate_id,
        const std::string& payload,
        size_t approximate_max_len);

    // 原子替换一个 Sorted Set，并设置 TTL。成员按 score 排序，用于保存某个
    // 冲突资源的列车 ETA 队列；空数组会删除旧集合而不创建空 Key。
    bool replaceSortedSet(
        const std::string& key,
        const std::vector<std::pair<double, std::string>>& scored_members,
        int ttl_ms);

    // 返回客户端当前是否已经至少拥有一个可用 Redis 连接。
    bool isEnabled() const;
    // 返回构建时是否检测到 hiredis；用于区分“未安装依赖”和“运行时不可达”。
    bool isCompiledWithRedis() const;
    // 返回无锁原子计数器的一致性较弱快照，适合监控展示。
    RedisCacheStats stats() const;
    // 禁止新操作并关闭池内全部连接；正在借出的连接归还时会被释放。
    void shutdown();

    // 析构时调用 shutdown，确保 hiredis 连接被释放。
    ~RedisCache();

    RedisCache(const RedisCache&) = delete;
    RedisCache& operator=(const RedisCache&) = delete;

private:
    // 使用单例，构造函数不对外开放。
    RedisCache();
    // PImpl 隔离 hiredis 类型，使未安装 hiredis 时头文件仍可编译。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
