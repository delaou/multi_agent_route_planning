#include "RedisCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#ifdef ENABLE_REDIS
#include <hiredis/hiredis.h>
#endif

struct RedisCache::Impl {
    std::string host;
    std::string password;
    std::string key_prefix;
    int port = 6379;
    int database = 0;
    int pool_size = 0;
    int connect_timeout_ms = 100;
    int command_timeout_ms = 20;
    int default_ttl_ms = 250;
    bool enabled = false;

    mutable std::mutex mutex;
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> bypasses{0};

#ifdef ENABLE_REDIS
    std::queue<redisContext*> available;

    // 把毫秒转换为 hiredis 使用的 timeval。
    static timeval toTimeval(int timeout_ms) {
        timeval result{};
        result.tv_sec = timeout_ms / 1000;
        result.tv_usec = (timeout_ms % 1000) * 1000;
        return result;
    }

    // Redis 的错误回复也有 reply 对象，因此必须同时检查类型。
    static bool replyOk(redisReply* reply) {
        return reply && reply->type != REDIS_REPLY_ERROR;
    }

    // 创建并配置单个连接，包括命令超时、AUTH 和逻辑数据库选择。
    redisContext* connectOne() const {
        const timeval connect_timeout = toTimeval(connect_timeout_ms);
        redisContext* context = redisConnectWithTimeout(
            host.c_str(), port, connect_timeout);
        if (!context || context->err) {
            if (context) redisFree(context);
            return nullptr;
        }

        const timeval command_timeout = toTimeval(command_timeout_ms);
        if (redisSetTimeout(context, command_timeout) != REDIS_OK) {
            redisFree(context);
            return nullptr;
        }

        if (!password.empty()) {
            redisReply* auth = static_cast<redisReply*>(redisCommand(
                context, "AUTH %b", password.data(), password.size()));
            const bool ok = replyOk(auth);
            if (auth) freeReplyObject(auth);
            if (!ok) {
                redisFree(context);
                return nullptr;
            }
        }

        if (database != 0) {
            redisReply* select = static_cast<redisReply*>(redisCommand(
                context, "SELECT %d", database));
            const bool ok = replyOk(select);
            if (select) freeReplyObject(select);
            if (!ok) {
                redisFree(context);
                return nullptr;
            }
        }

        return context;
    }

    // 非阻塞借用一个连接。高峰期池为空时直接降级，避免 HTTP 线程等待 Redis。
    redisContext* acquire() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled || available.empty()) {
            ++bypasses;
            return nullptr;
        }
        redisContext* context = available.front();
        available.pop();
        return context;
    }

    // 归还健康连接；故障连接会先销毁并尝试补建，避免把坏连接放回池中。
    void release(redisContext* context, bool healthy) {
        if (!context) return;
        if (!healthy || context->err) {
            redisFree(context);
            context = connectOne();
            if (!context) ++errors;
        }

        if (!context) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (enabled) available.push(context);
        else redisFree(context);
    }

    // 调用者已持有 mutex；关闭所有当前空闲连接。
    void closeAllLocked() {
        while (!available.empty()) {
            redisFree(available.front());
            available.pop();
        }
    }
#endif

    // 为所有业务 Key 加统一命名空间，避免同一 Redis 实例中的项目互相覆盖。
    std::string fullKey(const std::string& key) const {
        return key_prefix + key;
    }
};

RedisCache& RedisCache::instance() {
    static RedisCache cache;
    return cache;
}

RedisCache::RedisCache() : impl_(std::make_unique<Impl>()) {}

RedisCache::~RedisCache() {
    shutdown();
}

bool RedisCache::init(const std::string& host,
                      int port,
                      const std::string& password,
                      int database,
                      int pool_size,
                      int connect_timeout_ms,
                      int command_timeout_ms,
                      int default_ttl_ms,
                      const std::string& key_prefix) {
    shutdown();
    if (host.empty() || port <= 0 || port > 65535 || database < 0 ||
        pool_size <= 0 || connect_timeout_ms <= 0 ||
        command_timeout_ms <= 0 || default_ttl_ms <= 0) {
        return false;
    }

    impl_->host = host;
    impl_->port = port;
    impl_->password = password;
    impl_->database = database;
    impl_->pool_size = pool_size;
    impl_->connect_timeout_ms = connect_timeout_ms;
    impl_->command_timeout_ms = command_timeout_ms;
    impl_->default_ttl_ms = default_ttl_ms;
    impl_->key_prefix = key_prefix;

#ifdef ENABLE_REDIS
    std::vector<redisContext*> connected;
    connected.reserve(static_cast<size_t>(pool_size));
    for (int i = 0; i < pool_size; ++i) {
        redisContext* context = impl_->connectOne();
        if (context) connected.push_back(context);
    }
    if (connected.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (redisContext* context : connected) impl_->available.push(context);
        impl_->enabled = true;
    }
    return true;
#else
    (void)key_prefix;
    return false;
#endif
}

std::optional<std::string> RedisCache::get(const std::string& key) {
#ifdef ENABLE_REDIS
    redisContext* context = impl_->acquire();
    if (!context) return std::nullopt;
    const std::string full_key = impl_->fullKey(key);
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        context, "GET %b", full_key.data(), full_key.size()));

    bool healthy = reply != nullptr && context->err == 0;
    std::optional<std::string> value;
    if (reply && reply->type == REDIS_REPLY_STRING) {
        value = std::string(reply->str, static_cast<size_t>(reply->len));
        ++impl_->hits;
    } else if (reply && reply->type == REDIS_REPLY_NIL) {
        ++impl_->misses;
    } else {
        ++impl_->errors;
        healthy = false;
    }
    if (reply) freeReplyObject(reply);
    impl_->release(context, healthy);
    return value;
#else
    (void)key;
    ++impl_->bypasses;
    return std::nullopt;
#endif
}

bool RedisCache::set(const std::string& key,
                     const std::string& value,
                     int ttl_ms) {
#ifdef ENABLE_REDIS
    redisContext* context = impl_->acquire();
    if (!context) return false;
    const std::string full_key = impl_->fullKey(key);
    const int effective_ttl = ttl_ms > 0 ? ttl_ms : impl_->default_ttl_ms;
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        context, "SET %b %b PX %d", full_key.data(), full_key.size(),
        value.data(), value.size(), effective_ttl));
    const bool ok = Impl::replyOk(reply) && context->err == 0;
    if (ok) ++impl_->writes;
    else ++impl_->errors;
    if (reply) freeReplyObject(reply);
    impl_->release(context, ok);
    return ok;
#else
    (void)key;
    (void)value;
    (void)ttl_ms;
    ++impl_->bypasses;
    return false;
#endif
}

bool RedisCache::setPersistent(const std::string& key,
                               const std::string& value) {
#ifdef ENABLE_REDIS
    redisContext* context = impl_->acquire();
    if (!context) return false;
    const std::string full_key = impl_->fullKey(key);
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        context, "SET %b %b", full_key.data(), full_key.size(),
        value.data(), value.size()));
    const bool ok = Impl::replyOk(reply) && context->err == 0;
    if (ok) ++impl_->writes;
    else ++impl_->errors;
    if (reply) freeReplyObject(reply);
    impl_->release(context, ok);
    return ok;
#else
    (void)key;
    (void)value;
    ++impl_->bypasses;
    return false;
#endif
}

bool RedisCache::remove(const std::string& key) {
#ifdef ENABLE_REDIS
    redisContext* context = impl_->acquire();
    if (!context) return false;
    const std::string full_key = impl_->fullKey(key);
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        context, "DEL %b", full_key.data(), full_key.size()));
    const bool ok = Impl::replyOk(reply) && context->err == 0;
    if (!ok) ++impl_->errors;
    if (reply) freeReplyObject(reply);
    impl_->release(context, ok);
    return ok;
#else
    (void)key;
    ++impl_->bypasses;
    return false;
#endif
}

void RedisCache::remove(std::initializer_list<const char*> keys) {
#ifdef ENABLE_REDIS
    redisContext* context = impl_->acquire();
    if (!context) return;

    std::vector<std::string> arguments;
    arguments.reserve(keys.size() + 1);
    arguments.emplace_back("DEL");
    for (const char* key : keys) {
        if (key) arguments.push_back(impl_->fullKey(key));
    }
    if (arguments.size() == 1) {
        impl_->release(context, true);
        return;
    }

    std::vector<const char*> argv;
    std::vector<size_t> argv_lengths;
    argv.reserve(arguments.size());
    argv_lengths.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.data());
        argv_lengths.push_back(argument.size());
    }

    redisReply* reply = static_cast<redisReply*>(redisCommandArgv(
        context, static_cast<int>(argv.size()), argv.data(),
        argv_lengths.data()));
    const bool ok = Impl::replyOk(reply) && context->err == 0;
    if (!ok) ++impl_->errors;
    if (reply) freeReplyObject(reply);
    impl_->release(context, ok);
#else
    (void)keys;
    ++impl_->bypasses;
#endif
}

std::optional<std::string> RedisCache::appendStream(
    const std::string& stream,
    const std::string& event_type,
    const std::string& aggregate_id,
    const std::string& payload,
    size_t approximate_max_len) {
#ifdef ENABLE_REDIS
    if (approximate_max_len == 0) {
        ++impl_->errors;
        return std::nullopt;
    }
    redisContext* context = impl_->acquire();
    if (!context) return std::nullopt;
    const std::string full_stream = impl_->fullKey(stream);
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        context,
        "XADD %b MAXLEN ~ %zu * event_type %b aggregate_id %b payload %b",
        full_stream.data(), full_stream.size(), approximate_max_len,
        event_type.data(), event_type.size(),
        aggregate_id.data(), aggregate_id.size(),
        payload.data(), payload.size()));
    const bool ok = reply && reply->type == REDIS_REPLY_STRING &&
                    context->err == 0;
    std::optional<std::string> event_id;
    if (ok) {
        event_id = std::string(reply->str, static_cast<size_t>(reply->len));
        ++impl_->writes;
    } else {
        ++impl_->errors;
    }
    if (reply) freeReplyObject(reply);
    impl_->release(context, ok);
    return event_id;
#else
    (void)stream;
    (void)event_type;
    (void)aggregate_id;
    (void)payload;
    (void)approximate_max_len;
    ++impl_->bypasses;
    return std::nullopt;
#endif
}

bool RedisCache::replaceSortedSet(
    const std::string& key,
    const std::vector<std::pair<double, std::string>>& scored_members,
    int ttl_ms) {
#ifdef ENABLE_REDIS
    if (ttl_ms <= 0) {
        ++impl_->errors;
        return false;
    }
    redisContext* context = impl_->acquire();
    if (!context) return false;
    const std::string full_key = impl_->fullKey(key);

    // MULTI/EXEC 保证读者只看到旧集合或完整新集合，不会观察到重建中间态。
    bool ok = redisAppendCommand(context, "MULTI") == REDIS_OK &&
              redisAppendCommand(context, "DEL %b", full_key.data(),
                                 full_key.size()) == REDIS_OK;
    for (const auto& item : scored_members) {
        if (!ok) break;
        ok = redisAppendCommand(context, "ZADD %b %.17g %b",
            full_key.data(), full_key.size(), item.first,
            item.second.data(), item.second.size()) == REDIS_OK;
    }
    if (ok && !scored_members.empty()) {
        ok = redisAppendCommand(context, "PEXPIRE %b %d",
            full_key.data(), full_key.size(), ttl_ms) == REDIS_OK;
    }
    if (ok) ok = redisAppendCommand(context, "EXEC") == REDIS_OK;

    const size_t reply_count = 3 + scored_members.size() +
        (scored_members.empty() ? 0 : 1);
    for (size_t i = 0; ok && i < reply_count; ++i) {
        void* raw_reply = nullptr;
        if (redisGetReply(context, &raw_reply) != REDIS_OK || !raw_reply) {
            ok = false;
            break;
        }
        redisReply* reply = static_cast<redisReply*>(raw_reply);
        if (reply->type == REDIS_REPLY_ERROR) ok = false;
        freeReplyObject(reply);
    }
    ok = ok && context->err == 0;
    if (ok) ++impl_->writes;
    else ++impl_->errors;
    impl_->release(context, ok);
    return ok;
#else
    (void)key;
    (void)scored_members;
    (void)ttl_ms;
    ++impl_->bypasses;
    return false;
#endif
}

bool RedisCache::isEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->enabled;
}

bool RedisCache::isCompiledWithRedis() const {
#ifdef ENABLE_REDIS
    return true;
#else
    return false;
#endif
}

RedisCacheStats RedisCache::stats() const {
    RedisCacheStats result;
    result.hits = impl_->hits.load();
    result.misses = impl_->misses.load();
    result.writes = impl_->writes.load();
    result.errors = impl_->errors.load();
    result.bypasses = impl_->bypasses.load();
    return result;
}

void RedisCache::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->enabled = false;
#ifdef ENABLE_REDIS
    impl_->closeAllLocked();
#endif
}
