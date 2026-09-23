# 压测与优化记录（T4.x）

> 对应 `TASKS.md` 的 **M4**。这个文件就是路线图要求的「README 含压测数据与优化前后对比」的数据来源，
> 最后把结论表搬进 `README.md` 即可。

## 纪律（不遵守就等于白测）

1. **只比同一台机器、同一个内核、同一个编译目标（`make release`）** 的数据。
2. **参数不许中途改**：标准配置固定为 `wrk -t4 -c100 -d30s --latency`，写进本文件后不再动。
   迭代时可以 `bash bench.sh -q 标签` 跑 5s 版，但**不要**把 quick 数据填进正式表。
3. **每次只改一个变量**，改完立刻测；测完马上记「改了什么、预期什么、实际什么」。
4. **看 P99，不看平均值**。平均值会把毛刺藏起来，而毛刺才是问题。
5. `strace -c` 的 calls 列和 wrk 的 QPS **一起**记录：这个项目的优化多半是「少一次 syscall / 少一次拷贝」。
6. WSL2 的数字是虚拟化网络栈下的结果，**只能同机前后比**，不要和公网机器比，也不要写进简历当绝对值。

## 环境（每次换环境要更新）

| 项 | 值 |
|---|---|
| 机器 | WSL2 / Windows 笔记本 |
| 内核 | `uname -r` → |
| CPU | `grep -m1 'model name' /proc/cpuinfo` → |
| 核心数 | `nproc` → |
| 编译器 | `gcc --version` → |
| 构建 | `make release`（-O2） |

## 结果总表

跑完 `bash bench.sh <标签>` 会自动往 `docs/bench/results.tsv` 追加一行；把行搬到这里：

| # | 标签 | 提交 | QPS | P50 | P99 | 相对基线 | 改了什么 |
|---|---|---|---|---|---|---|---|
| 1 | baseline | | | | | 1.00× | 无（阻塞式/第一版事件循环） |
| 2 | | | | | | | |
| 3 | | | | | | | |

**syscall 对比**（`bash bench.sh -s <标签>` 会一起抓）：

| 标签 | `read` calls | `write` calls | `epoll_wait` calls | 说明 |
|---|---|---|---|---|
| baseline | | | | |
| | | | | |

## 每轮优化的记录模板（复制一份填）

### 第 N 轮：<一句话假设>

- **假设**：例如「响应头与文件体分两次 `write` → 每次请求多一次 syscall，改用 `sendfile`/`writev` 应该能降」
- **改动**：<改了哪个文件哪一段，commit>
- **预期**：<QPS 涨多少 / syscall 降多少>
- **实测**：<填表>
- **结论**：设想对/错？下一步做什么？（**错了也要写下来**，这是技术故事里最值钱的部分）

## 瓶颈在哪里（T4.2 的产出）

WSL2 上 `perf` 用不了（内核 `6.6.87.2-microsoft-standard-WSL2` 没有对应 linux-tools 包，VM 拿不到 PMU）。
替代方案：

```bash
bash bench.sh -s baseline          # 系统调用分布：哪一类调用占了大头
valgrind --tool=callgrind ./build/debug/mini-httpd 8080   # 函数级热点（慢，但准）
```

- top1 热点：
- top2 热点：
- top3 热点：
- 结论：当前是 **CPU 受限 / syscall 受限 / 内存拷贝受限**？（选一个并给证据）

## 常见瓶颈的对照（做完一轮就勾一个）

- [ ] 每请求多次 `write`（响应头 + 文件体分开）→ `writev` 或 `sendfile`
- [ ] 每请求 `malloc/free`（响应缓冲）→ 栈上缓冲 / 连接对象池
- [ ] 文件内容 `read` 进用户态再 `write` → `sendfile` 零拷贝
- [ ] `epoll_wait` 空转（`EPOLLOUT` 常驻）→ 只在写不完时注册
- [ ] 每次修改 fd 都调 `epoll_ctl` → 合并事件掩码（`EPOLLIN|EPOLLOUT` 一次设好）
- [ ] 小响应体也走大缓冲 → 自适应缓冲
