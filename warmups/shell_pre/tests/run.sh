#!/usr/bin/env bash
# tests/run.sh — mini-shell 回归测试
#   bash tests/run.sh                # 测默认的 build/debug/shell
#   bash tests/run.sh ./shell        # 测指定路径
#   STAGE=2 bash tests/run.sh        # 连管道/重定向用例一起跑（实现后再用）
set -u
set -o pipefail     # 必须：否则 $? 拿到的是 sed 的退出码，timeout 的 124 会被吞掉

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=${1:-$ROOT/build/debug/shell}
TMO=${TMO:-5}       # 单条用例超时（秒）
STAGE=${STAGE:-1}

# 下面会 cd 进临时目录，BIN 必须是绝对路径，否则相对路径立刻失效
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "找不到可执行文件: $BIN （先 make）" >&2; exit 2; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 2

pass=0; fail=0

# 提示符 '>' 是父进程 printf 出来的，且会跨 fork 留在 stdio 缓冲区里，
# 子进程 exit() 时把 '>>>' 一起冲出来，粘在错误信息前面。
# main.c 改成 isatty 判断后这里就是空操作，但留着可以让旧二进制也能测。
strip_prompt() { sed 's/^>\+//'; }

# ---- 精确比对 stdout ----
# case_run <名字> <输入> <期望输出>
case_run() {
    local name=$1 input=$2 want=$3 got rc
    got=$(printf '%s' "$input" | timeout "$TMO" "$BIN" 2>&1 | head -c 65536 | strip_prompt)
    rc=$?
    if [ "$rc" -eq 124 ]; then
        printf 'FAIL  %-24s 超时 %ss（多半是管道 fd 没关，EOF 到不了读端）\n' "$name" "$TMO"
        fail=$((fail+1)); return
    fi
    if [ "$got" = "$want" ]; then
        printf 'PASS  %-24s\n' "$name"; pass=$((pass+1))
    else
        printf 'FAIL  %-24s\n' "$name"; fail=$((fail+1))
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/      /'
    fi
}

# ---- 比对 stdout + 某个产出文件 ----
# case_file <名字> <输入> <期望 stdout> <产出文件> <期望文件内容>
case_file() {
    local name=$1 input=$2 want=$3 f=$4 wantf=$5 got gotf
    rm -f "$f"
    got=$(printf '%s' "$input" | timeout "$TMO" "$BIN" 2>&1 | head -c 65536 | strip_prompt)
    gotf=$(cat "$f" 2>/dev/null)
    if [ "$got" = "$want" ] && [ "$gotf" = "$wantf" ]; then
        printf 'PASS  %-24s\n' "$name"; pass=$((pass+1))
    else
        printf 'FAIL  %-24s stdout=[%s] 期望[%s] | %s=[%s] 期望[%s]\n' \
               "$name" "$got" "$want" "$f" "$gotf" "$wantf"
        fail=$((fail+1))
    fi
}

# ---- 正则匹配（pid 这类不确定输出）----
# case_regex <名字> <输入> <扩展正则>
case_regex() {
    local name=$1 input=$2 re=$3 got
    got=$(printf '%s' "$input" | timeout "$TMO" "$BIN" 2>&1 | head -c 65536 | strip_prompt)
    if printf '%s' "$got" | grep -Eq "$re"; then
        printf 'PASS  %-24s\n' "$name"; pass=$((pass+1))
    else
        printf 'FAIL  %-24s 期望匹配 /%s/，实际=[%s]\n' "$name" "$re" "$got"; fail=$((fail+1))
    fi
}

# ---- 后台必须立刻返回 ----
bg_check() {
    local name="后台立刻返回" t0 t1 ms
    t0=$(date +%s%N)
    printf 'sleep 3 &\nquit\n' | "$BIN" > bg.out 2>&1
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -lt 1500 ] && grep -Eq '^>?[0-9]+ sleep 3 &' bg.out; then
        printf 'PASS  %-24s (%sms)\n' "$name" "$ms"; pass=$((pass+1))
    else
        printf 'FAIL  %-24s 耗时 %sms（>=1500 说明 & 被当前台跑了），bg.out=[%s]\n' \
               "$name" "$ms" "$(cat bg.out)"
        fail=$((fail+1))
    fi
}

echo "被测程序: $BIN    STAGE=$STAGE"
echo "---- 单命令 ----"
case_run "echo 基本"        'echo hello
quit
' 'hello'
case_run "多参数"           'echo a b c
quit
' 'a b c'
case_run "连续空格折叠"      'echo     spaced
quit
' 'spaced'
case_run "空行/纯空格行"     '

   
echo after
quit
' 'after'
case_run "quit 立即退出"     'quit
echo nope
' ''
case_run "单独 & 被忽略"     '&
echo after
quit
' 'after'
case_run "命令不存在"        'no_such_cmd_xyz
quit
' 'no_such_cmd_xyz: Command not found.'
case_run "末行无换行符"      'echo tail' 'tail'
case_regex "后台打印 pid"    'sleep 3 &
quit
' '^[0-9]+ sleep 3 &'
bg_check

if [ "$STAGE" -ge 2 ]; then
    echo "---- 重定向 ----"
    case_file "> 覆盖写"      'echo hi > out.txt
quit
' '' out.txt 'hi'
    case_file ">> 追加"       'echo a > f
echo b >> f
quit
' '' f 'a
b'
    case_run  "< 输入"        'echo a > f
cat < f
quit
' 'a'
    case_run  "不污染父进程 fd" 'echo hi > f
echo still-here
quit
' 'still-here'
    echo "---- 管道 ----"
    case_run  "两段管道"       'echo a | cat
quit
' 'a'
    printf 'x\ny\n' > f
    case_run  "三段管道"       'cat f | wc -l
quit
' '2'
    case_run  "管道 EOF 传播"  'yes | head -1
quit
' 'y'
fi

echo "----------------"
printf '共 %d 条，通过 %d，失败 %d\n' "$((pass+fail))" "$pass" "$fail"
[ "$fail" -eq 0 ]
