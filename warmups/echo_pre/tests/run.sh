#!/usr/bin/env bash
#tests/run.sh
set -u 
set -o pipefail
set -x # 显示命令，方便学习

# 找到仓库根目录
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${1:-$ROOT/build/debug/server}

# 检查程序存在且可执行
[ -x "$BIN" ] || { echo "找不到可执行文件: ${BIN}  (先make debug)" >&2; exit 2;}

# 建一个临时工作目录，脚本退出时自动删除
work=$(mktemp -d) || exit 2
SRV=""
cleanup() {
    [ -n "$SRV" ] && kill -9 "$SRV" 2>/dev/null;
    rm -rf "$work";
}
trap cleanup EXIT

# 启动服务端（后台跑）, 日志写到临时目录
PORT=8800
"$BIN" 127.0.0.1 $PORT > "$work/srv.log" 2>&1 &
SRV=$!
sleep 0.3 # 给一点时间bind + listen

# 发hi给服务器，把服务器的内容存进got
got=$(printf 'hi\n' | timeout 10 nc -N 127.0.0.1 $PORT)

# 比对
if [ "$got" = "hi" ]; then
    echo "PASS 回显内容:$got"
    exit 0
else
    echo "FAIL 期望[hi] 实际[$got]"
    echo "---- 服务器日志----"
    cat "$work/srv.log"
    exit 1
fi

