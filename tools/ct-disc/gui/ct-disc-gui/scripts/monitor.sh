#!/bin/sh
#
# monitor.sh — AIPC device health monitor (persistent + remote forward)
# Usage:  ./monitor.sh [interval_sec] [log_dir] [remote_ip:port]
#         Default: 10s, /data/monitor, no remote forward
#
# Deploy:  scp monitor.sh root@192.0.2.72:/data/ && ssh root@192.0.2.72 "chmod +x /data/monitor.sh && nohup /data/monitor.sh 10 /data/monitor 192.0.2.100:9999 &"
#

INTERVAL="${1:-10}"
LOG_DIR="${2:-/data/monitor}"
REMOTE="${3:-}"              # ip:port for UDP forward, empty = disabled
PID_FILE="${LOG_DIR}/monitor.pid"
CSV_FILE="${LOG_DIR}/metrics.csv"
SUMMARY_FILE="${LOG_DIR}/summary.txt"
MAX_CSV_LINES=10000

mkdir -p "$LOG_DIR" || exit 1
echo $$ > "$PID_FILE"

# --- header ---
write_header() {
  echo "timestamp,uptime_s,load_1m,load_5m,mem_total_kb,mem_avail_kb,swap_used_kb,fd_camerad,fd_discovery,fd_eventbus,fd_airuntime,fd_total,disk_use_pct,inode_use_pct,procs_total,procs_blocked,goroutine_discovery,goroutine_eventbus,goroutine_airuntime,oom_score_camerad"
}

if [ ! -f "$CSV_FILE" ] || [ "$(wc -l < "$CSV_FILE")" -lt 1 ]; then
  write_header > "$CSV_FILE"
fi

# --- rotate CSV ---
rotate_csv() {
  if [ "$(wc -l < "$CSV_FILE")" -gt "$MAX_CSV_LINES" ]; then
    local ts=$(date +%Y%m%d_%H%M%S)
    mv "$CSV_FILE" "${LOG_DIR}/metrics_${ts}.csv"
    write_header > "$CSV_FILE"
    echo "[$(date)] rotated → metrics_${ts}.csv" >> "$LOG_DIR/monitor.log"
  fi
}

# --- collect one row ---
collect() {
  local uptime_s=$(awk '{printf "%.0f", $1}' /proc/uptime 2>/dev/null)
  local load=$(cat /proc/loadavg 2>/dev/null)
  local load1=$(echo "$load" | awk '{print $1}')
  local load5=$(echo "$load" | awk '{print $2}')

  local meminfo=$(cat /proc/meminfo 2>/dev/null)
  local mem_total=$(echo "$meminfo" | awk '/^MemTotal:/{print $2+0}')
  local mem_avail=$(echo "$meminfo" | awk '/^MemAvailable:/{print $2+0}')
  local swap_used=0
  local swap_total=$(echo "$meminfo" | awk '/^SwapTotal:/{print $2+0}')
  local swap_free=$(echo "$meminfo" | awk '/^SwapFree:/{print $2+0}')
  [ -n "$swap_total" ] && [ "$swap_total" -gt 0 ] && swap_used=$((swap_total - swap_free))

  local fd_camerad=0 fd_discovery=0 fd_eventbus=0 fd_airuntime=0
  local pid_camerad=$(pgrep -f "camera-daemon" 2>/dev/null | head -1)
  local pid_discovery=$(pgrep -f "device-discovery" 2>/dev/null | head -1)
  local pid_eventbus=$(pgrep -f "event-bus" 2>/dev/null | head -1)
  local pid_airuntime=$(pgrep -f "ai-runtime" 2>/dev/null | head -1)
  [ -n "$pid_camerad" ] && fd_camerad=$(ls "/proc/${pid_camerad}/fd" 2>/dev/null | wc -l)
  [ -n "$pid_discovery" ] && fd_discovery=$(ls "/proc/${pid_discovery}/fd" 2>/dev/null | wc -l)
  [ -n "$pid_eventbus" ] && fd_eventbus=$(ls "/proc/${pid_eventbus}/fd" 2>/dev/null | wc -l)
  [ -n "$pid_airuntime" ] && fd_airuntime=$(ls "/proc/${pid_airuntime}/fd" 2>/dev/null | wc -l)

  local fd_total=$(cat /proc/sys/fs/file-nr 2>/dev/null | awk '{print $1}')

  local disk_pct=0 inode_pct=0
  for mp in /data /opt/aipc /; do
    local d=$(df "$mp" 2>/dev/null | awk 'NR==2{print $5, $6}')
    if [ -n "$d" ]; then
      disk_pct=$(echo "$d" | awk '{print $1}' | tr -d '%')
      inode_pct=$(df -i "$mp" 2>/dev/null | awk 'NR==2{print $5}' | tr -d '%')
      break
    fi
  done

  local procs_total=$(cat /proc/loadavg 2>/dev/null | awk -F'/' '{print $2}' | awk '{print $1}')
  local procs_blocked=$(ps -eo stat= 2>/dev/null | grep -c '^D')

  local goroutine_discovery=0 goroutine_eventbus=0 goroutine_airuntime=0
  [ -n "$pid_discovery" ] && goroutine_discovery=$(awk '/^Threads:/{print $2+0}' "/proc/${pid_discovery}/status" 2>/dev/null)
  [ -n "$pid_eventbus" ] && goroutine_eventbus=$(awk '/^Threads:/{print $2+0}' "/proc/${pid_eventbus}/status" 2>/dev/null)
  [ -n "$pid_airuntime" ] && goroutine_airuntime=$(awk '/^Threads:/{print $2+0}' "/proc/${pid_airuntime}/status" 2>/dev/null)

  local oom_camerad=0
  [ -n "$pid_camerad" ] && oom_camerad=$(cat "/proc/${pid_camerad}/oom_score" 2>/dev/null)

  local ts=$(date -Iseconds 2>/dev/null || date +%Y-%m-%dT%H:%M:%S)

  echo "${ts},${uptime_s},${load1},${load5},${mem_total},${mem_avail},${swap_used},${fd_camerad},${fd_discovery},${fd_eventbus},${fd_airuntime},${fd_total},${disk_pct},${inode_pct},${procs_total},${procs_blocked},${goroutine_discovery},${goroutine_eventbus},${goroutine_airuntime},${oom_camerad}"
}

# --- forward row via UDP ---
forward() {
  local row="$1"
  if [ -n "$REMOTE" ] && command -v nc >/dev/null 2>&1; then
    # non-blocking: send and forget, don't let network stall block collection
    local host="${REMOTE%:*}"
    local port="${REMOTE##*:}"
    printf '%s\n' "$row" | nc -u -w0 "$host" "$port" 2>/dev/null || true
  fi
}

# --- print summary ---
print_summary() {
  local row="$1"
  IFS=',' read -r ts up_s l1 l5 mem_t mem_a swap fd_cam fd_disc fd_eb fd_ai fd_all disk_pct inode_pct procs blocked g_disc g_eb g_ai oom <<EOF
$row
EOF

  cat > "$SUMMARY_FILE" <<EOF2
══════════════════════════════════════════════════════════════
  AIPC Monitor  |  $(date)  |  interval=${INTERVAL}s
══════════════════════════════════════════════════════════════

  Uptime:     ${up_s}s   Load: ${l1} / ${l5} / ...
  Memory:     Avail=$(( mem_a / 1024 ))MB / Total=$(( mem_t / 1024 ))MB   Swap: ${swap}KB
  Disk:       Used ${disk_pct}%   Inodes: ${inode_pct}%
  Processes:  ${procs} total  |  ${blocked} blocked (D-state)

  PROCESS               PID    FD          THREADS
  --------------------  ------ ----------  ----------
  camera-daemon         ${pid_camerad:-N/A}     ${fd_cam:-0}
  device-discovery      ${pid_discovery:-N/A}     ${fd_disc:-0}         ${g_disc:-0}
  event-bus             ${pid_eventbus:-N/A}     ${fd_eb:-0}          ${g_eb:-0}
  ai-runtime            ${pid_airuntime:-N/A}     ${fd_ai:-0}          ${g_ai:-0}

  System FD:  ${fd_all} used
  Camera OOM: score=${oom} (higher = more likely to be killed)

  Log:     ${CSV_FILE}
  Alerts:  ${LOG_DIR}/alerts.log
  Remote:  ${REMOTE:-disabled}
══════════════════════════════════════════════════════════════
EOF2
}

# --- cleanup ---
cleanup() {
  echo "[$(date)] monitor stopped" >> "$LOG_DIR/monitor.log"
  rm -f "$PID_FILE"
}
trap cleanup EXIT INT TERM

echo "[$(date)] monitor started (PID=$$), interval=${INTERVAL}s, log=${CSV_FILE}, remote=${REMOTE:-none}" | tee "$LOG_DIR/monitor.log"

# --- main loop ---
while true; do
  ROW=$(collect)

  # write to persistent storage + sync
  echo "$ROW" >> "$CSV_FILE"
  sync "$CSV_FILE" 2>/dev/null || sync

  # forward to remote collector (survives device crash)
  forward "$ROW"

  rotate_csv
  print_summary "$ROW"

  # parse key fields for alerts
  OIFS="$IFS"; IFS=','; set -- $ROW; IFS="$OIFS"
  mem_a="$6"
  fd_cam="$8"
  fd_all="$11"
  disk_pct="$12"
  blocked="$16"

  if [ "$mem_a" -lt 51200 ] 2>/dev/null; then
    echo "[$(date)] ⚠️  MEMORY CRITICAL: available=${mem_a}KB ($((mem_a/1024))MB)" | tee -a "$LOG_DIR/alerts.log"
  fi

  if [ "$disk_pct" -gt 90 ] 2>/dev/null; then
    echo "[$(date)] ⚠️  DISK CRITICAL: ${disk_pct}% used" | tee -a "$LOG_DIR/alerts.log"
  fi

  if [ "$fd_cam" -gt 5000 ] 2>/dev/null; then
    echo "[$(date)] ⚠️  FD GROWTH: camera-daemon has ${fd_cam} open FDs" | tee -a "$LOG_DIR/alerts.log"
  fi

  if [ "$fd_all" -gt 40000 ] 2>/dev/null; then
    echo "[$(date)] ⚠️  FD EXHAUSTION: system-wide ${fd_all} FDs open" | tee -a "$LOG_DIR/alerts.log"
  fi

  if [ "$blocked" -gt 5 ] 2>/dev/null; then
    echo "[$(date)] ⚠️  BLOCKED PROCESSES: ${blocked} in D-state" | tee -a "$LOG_DIR/alerts.log"
    ps -eo pid,stat,wchan:32,comm 2>/dev/null | grep '^[[:space:]]*[0-9]* D' >> "$LOG_DIR/alerts.log"
  fi

  sleep "$INTERVAL"
done
