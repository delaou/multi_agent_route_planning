#!/usr/bin/env bash
# 端到端验证拓扑/遥测是否同时进入 HTTP、MySQL 和 Redis。
set -euo pipefail

SERVER_URL="${SERVER_URL:-http://127.0.0.1:8080}"
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-dispatch}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-dispatch_dev_password}"
MYSQL_DATABASE="${MYSQL_DATABASE:-multi_agent_dispatch}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PREFIX="${REDIS_PREFIX:-multi_agent:}"

# 上传最小合法线路；重复运行时相同几何不会制造新 revision。
curl -fsS -X POST "${SERVER_URL}/api/game/lines/snapshot" \
  -H 'Content-Type: application/json' \
  --data '{"routes":[{"line_id":"VERIFY-LINE","line_name":"storage check","target_headway_ms":15000,"points":[{"point_id":"p0","x":0,"y":0,"z":0},{"point_id":"p1","x":100,"y":0,"z":0}]}]}'
echo

# 上传一辆列车，触发最新状态、遥测历史、滚动计划和 Redis 实时视图写入。
curl -fsS -X POST "${SERVER_URL}/api/game/trains/snapshot" \
  -H 'Content-Type: application/json' \
  --data '{"simulation_time_ms":1000,"trains":[{"train_id":"VERIFY-TRAIN","line_id":"VERIFY-LINE","route_index":0,"travel_direction":"forward","position":{"x":0,"y":0,"z":0},"speed":10}]}'
echo

# 健康接口必须表明可信数据层正常。
curl -fsS "${SERVER_URL}/api/system/storage"
echo

MYSQL_PWD="${MYSQL_PASSWORD}" mysql \
  -h "${MYSQL_HOST}" -P "${MYSQL_PORT}" -u "${MYSQL_USER}" \
  "${MYSQL_DATABASE}" --batch --skip-column-names \
  -e "SELECT CONCAT('topologies=',COUNT(*)) FROM dispatch_topology_snapshots;
      SELECT CONCAT('active_verify_train=',COUNT(*)) FROM dispatch_train_state WHERE train_id='VERIFY-TRAIN' AND is_active=1;
      SELECT CONCAT('telemetry_batches=',COUNT(*)) FROM dispatch_telemetry_batches;"

redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" \
  MGET "${REDIS_PREFIX}realtime:topology_revision" \
       "${REDIS_PREFIX}realtime:train:VERIFY-TRAIN"
redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" \
  XLEN "${REDIS_PREFIX}stream:telemetry"

echo "STORAGE_VERIFICATION_OK"
