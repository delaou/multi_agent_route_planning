#!/usr/bin/env bash
set -euo pipefail

base_url="${BASE_URL:-http://127.0.0.1:8080}"
redis_host="${REDIS_HOST:-127.0.0.1}"
redis_port="${REDIS_PORT:-6379}"
key_prefix="${REDIS_KEY_PREFIX:-multi_agent:}"

echo "Cache status before requests:"
curl --fail --silent --show-error "${base_url}/api/system/cache"
echo

echo "Requesting the vehicle endpoint twice..."
curl --fail --silent --show-error "${base_url}/api/game/vehicles" >/dev/null
curl --fail --silent --show-error "${base_url}/api/game/vehicles" >/dev/null

echo "Cache status after requests:"
curl --fail --silent --show-error "${base_url}/api/system/cache"
echo

echo "Redis cache keys:"
redis-cli -h "${redis_host}" -p "${redis_port}" \
  --scan --pattern "${key_prefix}http:*"

echo "Vehicle cache TTL (milliseconds):"
redis-cli -h "${redis_host}" -p "${redis_port}" \
  PTTL "${key_prefix}http:vehicles"

