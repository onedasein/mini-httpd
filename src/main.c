/* src/main.c — 构建自检占位（T1.0b / T1.0c 用），T1.2e 起被真正的 listener 整个替换
 *
 * 它只证明这条链是通的：根 Makefile → 严格告警集 → build/<profile>/mini-httpd 能编能跑。
 *
 * 真正的程序契约（见 TASKS.md「M1 验收契约」）：
 *   argv  : <ip> <port> <www_root>（都有默认值：127.0.0.1 8080 tests/www）
 *   启动行: 先往 stdout 打一行含 "listening on" 的信息，再进 accept 循环
 * 所以这里**故意不打印 listening on** —— 现在什么都没监听，
 * 免得 e2e 脚本（靠这句话判断服务是否就绪）误以为服务起来了。
 */
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    puts("mini-httpd: scaffold build OK —— 还没有 listener（见 TASKS.md T1.2e）");
    return 0;
}
