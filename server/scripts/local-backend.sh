#!/usr/bin/env bash
set -euo pipefail
action=${1:?action required}
port=${2:?port required}
[[ "$port" =~ ^[0-9]+$ ]] && ((port > 1024 && port < 65536))
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
server_dir=$(pwd -P)
mkdir -p .local-launcher
exec 9>".local-launcher/backend-${port}.lock"
flock -w 5 9
pidfile=".local-launcher/backend-${port}.pid"
pid=0
ticks=0
[[ ! -f "$pidfile" ]] || read -r pid ticks < "$pidfile"
owned_running() {
    [[ "$pid" =~ ^[0-9]+$ && "$pid" -gt 1 && -r "/proc/$pid/stat" ]] || return 1
    [[ "$(awk '{print $22}' "/proc/$pid/stat")" == "$ticks" ]] || return 1
    exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
    [[ "$exe" == "$server_dir/bin/app" ||
       "$exe" == "$server_dir/bin/app (deleted)" ]] || return 1
    [[ "$(readlink -f "/proc/$pid/cwd" 2>/dev/null || true)" == "$server_dir" ]]
}
case "$action" in
  status)
    if owned_running; then echo "RUNNING $pid"; else echo STOPPED; fi
    ;;
  start)
    if owned_running; then echo "RUNNING $pid"; exit 0; fi
    [[ -x bin/app ]] || { echo 'Build server/bin/app first with make.' >&2; exit 1; }
    [[ -f config/config.yaml ]] || cp config/config.example.yaml config/config.yaml
    if ss -ltnH "sport = :$port" | grep -q .; then
      echo "Backend port $port belongs to an unmanaged listener; refusing to replace it." >&2
      exit 1
    fi
    nohup ./bin/app -p "$port" >> ".local-launcher/backend-${port}.log" 2>&1 < /dev/null 9>&- &
    pid=$!
    ticks=$(awk '{print $22}' "/proc/$pid/stat")
    printf '%s %s\n' "$pid" "$ticks" > "$pidfile"
    echo "STARTED $pid"
    ;;
  stop)
    if owned_running; then
      kill -TERM "$pid"
      for _ in {1..20}; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
      done
      if kill -0 "$pid" 2>/dev/null; then
        echo "Backend $pid did not stop after SIGTERM." >&2
        exit 1
      fi
      rm -f -- "$pidfile"
      echo "STOPPED $pid"
    else
      rm -f -- "$pidfile"
      echo STOPPED
    fi
    ;;
  *) echo 'Expected start/status/stop' >&2; exit 1 ;;
esac
