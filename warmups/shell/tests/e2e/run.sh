#!/usr/bin/env bash
# tests/e2e/run.sh —— mini shell 端到端：起完整二进制、喂 stdin、脚本比对
#
# 用法：bash tests/e2e/run.sh [二进制路径]
#   Makefile 里 e2e 传 ./build/debug/shell，e2e-asan 传 ./build/asan/shell —— 同一套断言双跑
#
# 这一层和 tests/unit/ 的分工：单元测试直接构造 struct command 调用函数；
# 这里只认「二进制 + stdin + 产物」，不碰内部结构。
set -u
set -o pipefail
# set -x   # 打开能看到每条命令，方便学习

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${1:-$ROOT/build/debug/shell}

[ -x "$BIN" ] || { echo "找不到可执行文件: ${BIN}（先 make debug）" >&2; exit 2; }
# 下面会 cd 到临时目录，所以先把二进制转成绝对路径
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

# work: 每条用例自己的沙盒（相对路径都落在这里，绝不污染仓库）
# logs: 收 stderr —— 必须放在 work 外面，否则它会出现在 ls 的清单里
work=$(mktemp -d) || exit 2
logs=$(mktemp -d) || exit 2
trap 'rm -rf "$work" "$logs"' EXIT

pass=0
fail=0
ok()  { printf 'PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL  %s\n' "$1"; fail=$((fail + 1)); }
eq()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1   —— 期望[$2] 实际[$3]"; fi; }

# 当前用例的沙盒；每条用例开头自己 mkdir
d=$work
mkdir -p "$d"

out=""
rc=0
# 跑一段 stdin 脚本（每个参数是一行），产物落在 $d
run() {
    out=$(cd "$d" && printf '%s\n' "$@" | timeout 10 "$BIN" 2>"$logs/stderr")
    rc=$?
}
prompts() { printf '%s' "$1" | tr -cd '>' | wc -c; }
# 断言提示符个数；不对就把 stderr 摊开，省得再复现一遍
check_prompts() {
    local got
    got=$(prompts "$out")
    if [ "$got" = "$2" ]; then
        ok "$1"
    else
        bad "$1   —— 期望 $2 个提示符，实际 $got（多出来的是「本该退出的子进程又跑回了主循环」）"
        sed 's/^/      | /' "$logs/stderr"
    fi
}

# ============================================================
# ① 验收命令：ls | wc -l > out.txt
#    先 touch out.txt：这个实现是「每条命令在自己的子进程里做重定向」，
#    out.txt 的创建和 ls 的 readdir 之间没有顺序保证 —— 预先建好就把竞争抹掉了，
#    条数才是确定的 6（5 个文件 + 它自己）。
#    （顺带也是和 bash 的差别：bash 在起管道之前就建好重定向文件。）
# ============================================================
d=$work/acceptance; mkdir -p "$d"
touch "$d/out.txt"
for i in 1 2 3 4 5; do : > "$d/f$i.txt"; done
run 'ls | wc -l > out.txt'
eq "验收命令 ls | wc -l > out.txt → 6 行" "6" "$(cat "$d/out.txt" 2>/dev/null)"

# ============================================================
# ② 管道真的把字节搬过去了
# ============================================================
d=$work/pipe; mkdir -p "$d"
run 'echo hello | wc -c > bytes.txt'
eq "echo hello | wc -c → 6 字节" "6" "$(cat "$d/bytes.txt" 2>/dev/null)"

# ============================================================
# ③ 三级管道 + 重定向
# ============================================================
d=$work/pipe3; mkdir -p "$d"
printf 'a\nb\nc\n' > "$d/lines.txt"
run 'cat lines.txt | cat | wc -l > n.txt'
eq "cat | cat | wc -l → 3 行" "3" "$(cat "$d/n.txt" 2>/dev/null)"

# ============================================================
# ④ 多参数 / 连续空白
# ============================================================
d=$work/argv; mkdir -p "$d"
run 'echo   a    b   c   > args.txt'
eq "echo   a    b   c   > args.txt → 「a b c」" "a b c" "$(cat "$d/args.txt" 2>/dev/null)"

# ============================================================
# ⑤ > 要截断（O_TRUNC），不是追加
# ============================================================
d=$work/trunc; mkdir -p "$d"
printf 'junk\njunk\njunk\n' > "$d/t.txt"
run 'echo hi > t.txt'
eq "echo hi > t.txt 覆盖旧内容" "hi" "$(cat "$d/t.txt" 2>/dev/null)"
eq "echo hi > t.txt 只剩 1 行" "1" "$(wc -l < "$d/t.txt" 2>/dev/null | tr -d ' ')"

# ============================================================
# ⑥ 一个会话里多行：上一行的命令数不能漏到下一行
#    （parse_line 不会清空 cmds[]，只看 *ncmds —— 这条盯的就是它）
# ============================================================
d=$work/multi; mkdir -p "$d"
run 'echo A | cat > f1.txt' 'echo B > f2.txt'
eq "多行会话 第 1 行 → f1.txt = A" "A" "$(cat "$d/f1.txt" 2>/dev/null)"
eq "多行会话 第 2 行 → f2.txt = B（没有把上一行的命令再跑一遍）" "B" "$(cat "$d/f2.txt" 2>/dev/null)"

# ============================================================
# ⑦ 不存在的命令：子进程 execvp 失败后必须退出
#    父进程只该为这一行打印一个提示符，再等下一行（EOF）→ 一共 2 个。
#    如果子进程「返回」而不是退出，它会回到 main 的循环里再打一个提示符（变成第二个 shell）。
# ============================================================
d=$work/execfail; mkdir -p "$d"
run 'no_such_command_xyz'
check_prompts "no_such_command_xyz → 只出现 2 个提示符（子进程没有变成第二个 shell）" 2

# ============================================================
# ⑧ 重定向目标打不开：同理，子进程 open 失败后也必须退出
#    （这是同一类 bug 的另一条路径：dup2/open 的失败分支）
# ============================================================
d=$work/openfail; mkdir -p "$d"
run 'echo hi > /no/such/dir/out.txt'
check_prompts "重定向到打不开的路径 → 只出现 2 个提示符" 2

# ============================================================
# ⑨ 空 stdin：一个提示符、正常退出、不挂死
# ============================================================
d=$work/eof; mkdir -p "$d"
out=$(cd "$d" && printf '' | timeout 10 "$BIN" 2>"$logs/stderr")
rc=$?
eq "空 stdin → 退出码 0（不是 timeout 的 124）" "0" "$rc"
check_prompts "空 stdin → 只有 1 个提示符" 1

# ============================================================
# ⑩ 连续空行：不挂死、退出码 0
#    （空行现在会报 syntax error: empty command. —— 那是待定的设计问题，这里只要求不挂）
# ============================================================
d=$work/blank; mkdir -p "$d"
run '' ''
eq "连续空行 → 退出码 0（不挂死）" "0" "$rc"

# ============================================================
printf '\n----------------------------------------\n'
printf 'e2e: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
