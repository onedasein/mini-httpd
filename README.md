# mini-httpd

用 C 从零写一个 **单进程、单线程、epoll 边沿触发（ET）、全程非阻塞**的 HTTP/1.1 静态文件服务器。

> **完整任务分解见 [`TASKS.md`](TASKS.md)**（M0–M5，每个任务都有预计时间和可执行的完成判据）。

## 目标

1. 监听端口、接受并发连接 → `wrk -t4 -c200 -d30s` 无崩溃
2. HTTP/1.1 请求解析（header 名大小写不敏感、超长拒绝）
3. 静态文件响应（`Content-Length` / `Content-Type` / 404 / 400）
4. **keep-alive**（同一 TCP 连接上处理多个请求）
5. 非阻塞 + ET（每次事件都读到/写到 `EAGAIN` 才收手）
6. 空闲连接 5s 超时断开
7. 安全：挡住 `..` 路径穿越；客户端突断不会让进程退出
8. 无泄漏：ASan/UBSan 全绿，fd 数在压测前后一致
9. 压测数据：QPS / P50 / P99 + 优化前后对比表

**第一版不涉及**：HTTPS、POST/PUT、CGI、动态内容、目录列表、gzip、HTTP/2、多线程/多进程、Range。

## 构建与运行

```bash
make debug      # -O0 -g3，开发用（gdb 友好）
make release    # -O2，压测必须用这个
make asan       # AddressSanitizer + UBSan，体检用
make run        # 编译并启动在 127.0.0.1:8080
make clean
```

```bash
./build/debug/mini-httpd 8080 &
curl -i http://127.0.0.1:8080/index.html
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/index.html
```

## 目录结构

```
mini-httpd/
├── TASKS.md      # 任务分解
├── bench.sh      # 压测脚本：固定参数、自动起服务、结果落盘（见 M4）
├── src/          # 源
├── www/          # 被测的静态文件根目录
├── docs/
│   ├── env.md    # 环境笔记（T0.1）
│   ├── bench.md  # 压测与优化记录（T4.x）
│   └── bench/    # 每次压测的原始输出 + results.tsv
└── build/        # 编译产物（已 gitignore）
```

## WSL2 注意事项

- **`perf` 装不上也用不了**：内核是 `6.6.87.2-microsoft-standard-WSL2`，Ubuntu 仓库里没有对应的
  `linux-tools-<版本>` 包，而且 WSL2 是虚拟机、拿不到 PMU 硬件计数器。
  → 优化阶段改用 **`strace -c`**（系统调用分布）和 `valgrind --tool=callgrind`（函数热点）。所做的优化里有一半是「减少 syscall / 减少拷贝」。
- **压测数字只做「前后相对比较」**：WSL2 的网络栈是虚拟化的，QPS 绝对值不能和外网机器比。
  README 里的数据请注明「WSL2 / 同机自测 / 仅作优化前后对比」。
- `SO_REUSEPORT`、`io_uring` 等在 WSL2 上表现和裸机不同。
