#include "WebServer.h"
#include "Config.h"
#include "DBConnectionPool.h"
#include "VehicleManager.h"
#include "RailwayDispatchService.h"
#include "RedisCache.h"
#include "DispatchPersistence.h"
#include <csignal>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace {
volatile std::sig_atomic_t stop_requested = 0;

void requestStop(int) {
    stop_requested = 1;
}

// 返回启动恢复使用的进程内单调时间。
uint64_t startupWallNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
LogLevel configuredLogLevel(const std::string& value) {
    if (value == "DEBUG") return LogLevel::DEBUG;
    if (value == "WARN") return LogLevel::WARN;
    if (value == "ERROR") return LogLevel::ERROR;
    return LogLevel::INFO;
}
}

int main(int argc, char* argv[]) {
    Config config;
    const std::filesystem::path working_config = "./config/config.yaml";
    const std::filesystem::path executable_config =
        std::filesystem::absolute(argv[0]).parent_path().parent_path() /
        "config" / "config.yaml";
    std::filesystem::path config_path;
    if (std::filesystem::exists(working_config)) {
        config_path = working_config;
    } else if (std::filesystem::exists(executable_config)) {
        config_path = executable_config;
    }
    if (!config_path.empty()) {
        if (!config.loadYaml(config_path.string())) {
            std::cerr << "Invalid config file: " << config_path << '\n';
            return 2;
        }
    } else {
        std::cerr << "Config file not found; using safe memory-only defaults\n";
    }
    config.parseArgs(argc, argv);

    Logger::instance().init(100000, config.log_path,
                            configuredLogLevel(config.log_level),
                            config.log_async);
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);

    // 必须在 WebServer 启动工作线程前完成，后续 HTTP 请求只读这套配置。
    RailwayDispatchService::instance().configure(
        config.railwayDispatchServiceConfig());

    if (config.redis_enabled) {
        RedisCache& redis = RedisCache::instance();
        if (!redis.init(config.redis_host,
                        config.redis_port,
                        config.redis_password,
                        config.redis_database,
                        config.redis_pool_size,
                        config.redis_connect_timeout_ms,
                        config.redis_command_timeout_ms,
                        config.redis_response_cache_ttl_ms,
                        config.redis_key_prefix)) {
            LOG_WARN(redis.isCompiledWithRedis()
                ? "Redis unavailable; response cache disabled and server will fail open"
                : "Redis requested but hiredis was not found at build time; cache disabled");
        } else {
            LOG_INFO("Redis response cache initialized");
        }
    }

    if (config.mysql_enabled) {
        DBConnectionPool* db_pool = DBConnectionPool::getInstance();
        if (!db_pool->init(
                config.mysql_host,
                config.mysql_user,
                config.mysql_password,
                config.mysql_db,
                config.mysql_port,
                config.mysql_pool_size)) {
            LOG_ERROR("MySQL connection pool init failed");
            return 1;
        }

        if (!VehicleManager::instance().enablePersistence(db_pool)) {
            LOG_ERROR("Vehicle persistence init failed");
            return 1;
        }

        LOG_INFO(
            "MySQL persistence initialized, loaded vehicles="
            + std::to_string(VehicleManager::instance().size())
        );
    } else {
        LOG_WARN("MySQL persistence disabled; vehicle data is memory-only");
    }

    DispatchPersistence dispatch_persistence;
    // Redis 连接失败时保持 fail-open；只在至少一个数据后端实际可用时绑定。
    if (config.mysql_enabled || RedisCache::instance().isEnabled()) {
        std::string persistence_error;
        DBConnectionPool* pool = config.mysql_enabled
            ? DBConnectionPool::getInstance() : nullptr;
        if (!dispatch_persistence.initialize(
                pool, &RedisCache::instance(),
                config.dispatchPersistenceConfig(), &persistence_error)) {
            LOG_ERROR("Dispatch persistence init failed: " + persistence_error);
            return 1;
        }
        if (!RailwayDispatchService::instance().enablePersistence(
                &dispatch_persistence, startupWallNowMs(),
                &persistence_error)) {
            LOG_ERROR("Dispatch recovery failed: " + persistence_error);
            return 1;
        }
        const DispatchPersistenceStats stats = dispatch_persistence.stats();
        LOG_INFO("Dispatch persistence initialized, command_recovery=" +
            std::string(stats.command_recovery_enabled ? "enabled" : "disabled") +
            " recovered pending=" +
            std::to_string(stats.recovered_pending_commands) +
            " active=" + std::to_string(stats.recovered_active_commands));
    }

    {
        WebServer server(
            config.port,
            config.thread_num,
            config.actor_model,
            config.timeout_ms
        );

        LOG_INFO("Server start");
        server.start(&stop_requested);
        LOG_INFO("Server stopping");
    }

    if (config.mysql_enabled) {
        if (!VehicleManager::instance().saveAll()) {
            LOG_ERROR("Failed to save vehicle snapshot to MySQL");
            return 1;
        }
        LOG_INFO(
            "Vehicle snapshot saved to MySQL, vehicles="
            + std::to_string(VehicleManager::instance().size())
        );
    }

    const RedisCacheStats redis_stats = RedisCache::instance().stats();
    LOG_INFO(
        "Redis cache stats hits=" + std::to_string(redis_stats.hits)
        + " misses=" + std::to_string(redis_stats.misses)
        + " writes=" + std::to_string(redis_stats.writes)
        + " errors=" + std::to_string(redis_stats.errors)
        + " bypasses=" + std::to_string(redis_stats.bypasses)
    );
    const DispatchPersistenceStats persistence_stats =
        dispatch_persistence.stats();
    LOG_INFO("Dispatch storage stats mysql_writes=" +
        std::to_string(persistence_stats.mysql_writes) +
        " mysql_errors=" + std::to_string(persistence_stats.mysql_errors) +
        " redis_writes=" + std::to_string(persistence_stats.redis_writes) +
        " redis_errors=" + std::to_string(persistence_stats.redis_errors));
    dispatch_persistence.shutdown();
    RedisCache::instance().shutdown();

    return 0;
}
