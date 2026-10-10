#!/usr/bin/env bash
# tests/e2e/run.sh — warmups/pc（T0.5 生产者-消费者）端到端验收
#   bash tests/e2e/run.sh                    # 测 build/debug/pc
#   bash tests/e2e/run.sh ./build/asan/pc    # 用 ASan 版跑同一套
#
# 判据（每个并发形状都必须满足）：
#   退出码 0 + consumed_count == P*items + consumed_sum == items*P*(P+1)/2
#   —— TSan 只保证「没有竞争访问」，保证不了「元素没丢/没重复」，这条才是真验收
set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${1:-$ROOT/build/debug/pc}
TMO=${TMO:-30}

BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN （先 make debug）" >&2; exit 2; }

pass=0; fail=0
ok()  { printf 'PASS  %-24s %s\n' "$1" "${2:-}"; pass=$((pass+1)); }
bad() { printf 'FAIL  %-24s %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
field() { printf '%s\n' "$1" | grep -o "$2=[0-9-]*" | head -1 | cut -d= -f2; }

# run <名字> <P> <C> <items> <cap>
run() {
    local name=$1 P=$2 C=$3 items=$4 cap=$5
    local out rc got want gsum wsum
    out=$(timeout "$TMO" "$BIN" "$P" "$C" "$items" "$cap" 2>&1); rc=$?
    if [ "$rc" -eq 124 ]; then
        bad "$name" "超时 ${TMO}s —— 死锁/挂起（消费者没等到毒丸？）"; return
    fi
    if [ "$rc" -ne 0 ]; then
        bad "$name" "退出码 $rc：$(printf '%s' "$out" | tr '\n' ' ')"; return
    fi
    got=$(field "$out" consumed_count);  want=$(( P * items ))
    gsum=$(field "$out" consumed_sum);   wsum=$(( items * P * (P + 1) / 2 ))
    if [ "$got" = "$want" ] && [ "$gsum" = "$wsum" ]; then
        ok "$name" "count=$got sum=$gsum"
    else
        bad "$name" "count=$got/$want sum=$gsum/$wsum"
    fi
}

# reject <名字> <期望退出码> <参数...>
reject() {
    local name=$1 wantrc=$2; shift 2
    local out rc
    out=$(timeout "$TMO" "$BIN" "$@" 2>&1); rc=$?
    if [ "$rc" -eq "$wantrc" ]; then
        ok "$name" "退出码 $rc"
    else
        bad "$name" "期望退出码 $wantrc，实际 $rc：$(printf '%s' "$out" | tr '\n' ' ')"
    fi
}

printf '被测程序: %s\n' "$BIN"
printf -- '---- 并发形状（P 生产者 / C 消费者）----\n'
run "1P1C cap=1024" 1 1 20000 1024
run "1P4C cap=1024" 1 4 20000 1024
run "4P1C cap=1024" 4 1 20000 1024
run "4P4C cap=1024" 4 4 20000 1024
run "4P4C cap=1"    4 4 20000 1
run "2P8C cap=1"    2 8 5000  1
run "1P1C items=0"  1 1 0     1

printf -- '---- 非法参数（main 兜住；pc_t_init 是 void，兜不住）----\n'
reject "cap=0"       2 1 1 100  0
reject "producers=0" 2 0 1 100  16
reject "consumers=0" 2 1 0 100  16
reject "items=-1"    2 1 1 -1   16

printf -- '----\n'
printf '共 %d 条，通过 %d，失败 %d\n' "$((pass + fail))" "$pass" "$fail"
[ "$fail" -eq 0 ]
