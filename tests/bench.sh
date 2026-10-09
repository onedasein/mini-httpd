#!/usr/bin/env bash
#
# mini-httpd 压测脚本 —— 参数固定，保证「优化前 / 优化后」的数据可比。
#
#   bash bench.sh <标签>          # 标准跑：wrk -t4 -c100 -d30s --latency
#   bash bench.sh -q <标签>       # 快速跑：-d5s（迭代时用，别进正式表）
#   bash bench.sh -s <标签>       # 同时抓 strace -c 的系统调用统计（WSL2 上 perf 的替代品）
#   PORT=8080 URL_PATH=/index.html bash bench.sh <标签>
#   SRV_CMD="python3 -m http.server --directory www" bash bench.sh <标签>   # 对照组/校准
#
# 产物：
#   docs/bench/<日期>-<标签>.txt   原始输出（含环境信息，可直接贴进 README）
#   docs/bench/results.tsv         每次一行汇总，用来生成对比表
#
set -uo pipefail
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")"

BIN="build/release/mini-httpd"
SRV_CMD="${SRV_CMD:-$BIN}"   # 可覆盖：例如和 python -m http.server 做对照
PORT="${PORT:-8080}"
URL_PATH="${URL_PATH:-/index.html}"
URL="http://127.0.0.1:${PORT}${URL_PATH}"
THREADS=4
CONN=100
DUR=30
QUICK=0
STRACE=0
LABEL=""

while [ $# -gt 0 ]; do
  case "$1" in
    -q|--quick)  QUICK=1; DUR=5; shift ;;
    -s|--strace) STRACE=1; shift ;;
    -h|--help)   sed -n '2,13p' "$SELF"; exit 0 ;;
    *)           LABEL="$1"; shift ;;
  esac
done
LABEL="${LABEL:-未命名}"
[ "$QUICK" = 1 ] && LABEL="${LABEL}-quick"

command -v wrk >/dev/null || { echo "缺少 wrk：sudo apt install wrk" >&2; exit 1; }

echo ">>> make release"
make release >/dev/null || { echo "make release 失败" >&2; exit 1; }
[ -x "$BIN" ] || { echo "没有 $BIN" >&2; exit 1; }

mkdir -p docs/bench
STAMP="$(date +%Y-%m-%d)"
RAW="docs/bench/${STAMP}-${LABEL}.txt"
TSV="docs/bench/results.tsv"

SRV_PID=""
cleanup() {
  if [ -n "$SRV_PID" ]; then
    kill "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
  fi
}
trap cleanup EXIT

# ---------- 起服务 ----------
# shellcheck disable=SC2086  # SRV_CMD 需要按空格拆成命令+参数
$SRV_CMD "$PORT" >/dev/null 2>&1 &
SRV_PID=$!

ready=0
for _ in $(seq 1 30); do
  if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "服务端一启动就退出了 —— 大概率 HTTP 还没实现（先做完 M1-T1.2 再来压测）。" >&2
    exit 1
  fi
  if curl -s -o /dev/null -m 1 "$URL"; then ready=1; break; fi
  sleep 0.2
done
if [ "$ready" != 1 ]; then
  echo "服务端在 $URL 上没有响应（检查路由 / 端口 / 是否还在阻塞 accept）。" >&2
  exit 1
fi

# 护栏：对着 404 压测出来的 QPS 没有意义
CODE="$(curl -s -o /dev/null -m 3 -w '%{http_code}' "$URL")"
if [ "$CODE" != "200" ]; then
  echo "压测目标 $URL 返回 $CODE（不是 200）。" >&2
  echo "确认 www/ 下有这个文件；确实要压非 200 路径就加 ALLOW_NON_200=1。" >&2
  [ "${ALLOW_NON_200:-0}" = "1" ] || exit 1
fi

# ---------- 预热（让 page cache 和连接都热起来，提升可比性）----------
wrk -t"$THREADS" -c"$CONN" -d3s "$URL" >/dev/null 2>&1

# ---------- 正式跑 ----------
{
  echo "=== mini-httpd bench ==="
  echo "标签     : $LABEL"
  echo "时间     : $(date '+%F %T %z')"
  echo "提交     : $(git rev-parse --short HEAD 2>/dev/null || echo -)"
  git diff --quiet 2>/dev/null || echo "工作区   : 有未提交改动（数据可能对应不到某个提交）"
  echo "参数     : wrk -t$THREADS -c$CONN -d${DUR}s --latency $URL"
  echo "内核     : $(uname -r)"
  echo "CPU      : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  echo "核心数   : $(nproc)"
  echo "编译器   : $(gcc --version | head -1)"
  echo "被测进程 : $SRV_CMD（默认 $BIN，-O2）"
  echo
  echo "--- wrk ---"
  wrk -t"$THREADS" -c"$CONN" -d"${DUR}s" --latency "$URL"

  if [ "$STRACE" = 1 ]; then
    echo
    if command -v strace >/dev/null; then
      echo "--- strace -c（8s 压测期间的系统调用统计）---"
      timeout 14 strace -c -p "$SRV_PID" 2>&1 &
      ST_PID=$!
      sleep 1
      wrk -t"$THREADS" -c"$CONN" -d8s "$URL" >/dev/null 2>&1
      wait "$ST_PID" 2>/dev/null
      echo
      echo "（提示：T4 的优化里「减少 syscall / 减少拷贝」就看这张表的 calls 列）"
    else
      echo "--- 跳过 strace：未安装（sudo apt install strace）---"
    fi
  fi
} 2>&1 | tee "$RAW"

# ---------- 汇总一行 ----------
QPS=$(awk '/^Requests\/sec:/{print $2; exit}' "$RAW")
P50=$(awk '/^ *50%/{print $2; exit}' "$RAW")
P90=$(awk '/^ *90%/{print $2; exit}' "$RAW")
P99=$(awk '/^ *99%/{print $2; exit}' "$RAW")
# Socket errors 行形如：  Socket errors: connect 0, read 0, write 0, timeout 18
SOCKERR=$(awk -F'[:,]' '/Socket errors/{s=0; for(i=2;i<=NF;i++){gsub(/[^0-9]/,"",$i); s+=$i} print s; exit}' "$RAW")
NON2XX=$(awk -F'[:,]' '/Non-2xx/{gsub(/[^0-9]/,"",$2); print $2; exit}' "$RAW")

if [ ! -f "$TSV" ]; then
  printf 'date\tlabel\tqps\tp50\tp90\tp99\tsock_err\tnon2xx\traw\n' > "$TSV"
fi
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$STAMP" "$LABEL" "${QPS:--}" "${P50:--}" "${P90:--}" "${P99:--}" \
  "${SOCKERR:-0}" "${NON2XX:-0}" "$RAW" >> "$TSV"

echo
echo "✅ 原始输出：$RAW"
echo "✅ 汇总追加：$TSV"
printf '   %s\n' "$(tail -1 "$TSV")"
echo
echo "下一步：把这一行填进 docs/bench.md 的对比表，并写清「改了什么、为什么」"
[ "${SOCKERR:-0}" != "0" ] && { echo; echo "⚠️  有 ${SOCKERR} 个 socket 错误 —— 这轮数据不可用于对比，先修好再测。"; }
