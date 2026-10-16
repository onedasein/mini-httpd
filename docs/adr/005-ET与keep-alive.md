# ADR-005：边沿触发（ET）与 keep-alive（M2 阶段 B 后半 + 阶段 C）

- 状态：**已实施**（2026-10-14），待本人复核
- 相关：ADR-001（单线程事件循环）、ADR-003（连接表）、ADR-004（LT 事件循环）、
  `CONTRACTS.md` §1b（连接语义真源）、§2（状态机不变式）、`docs/bench/M2-ET验收.md`

## 背景

ADR-004 的 LT 版已经把"慢客户端拖死全服"治好了（实测 000 → 200），但那时：
一次事件只做一段 IO、每个事件都 `epoll_ctl(MOD)`、响应头一律 `Connection: close`。
阶段 B 后半与阶段 C 要收掉这三笔：切 ET、按需挂 `EPOLLOUT`、上 keep-alive。

## 决定

1. **全部 fd 切 `EPOLLET`，每个事件把当前步骤做到 `IO_WOULD_BLOCK` 为止。**
   这是 ET 唯一的硬要求（也是唯一容易写错的地方）：边沿触发只在**状态变化**时通知一次，
   你要是半路回事件循环，剩下的数据再也没人叫 —— 表现为"连接偶尔挂住、没有任何报错"。
   落地成 `conn_pump()` 那个循环：`run_step()` 返回"搬了多少字节"，0 才收工去登记事件。
   监听 fd 同理：`on_accept()` 必须 `accept` 到 `EAGAIN`（否则连接堆在队列里，`ss -tln` 的 `Recv-Q` 不归零）。
2. **`EPOLLOUT` 只在真的写不动之后才挂上，写完立刻摘掉**（T2.4c）。
   做法是把"写"放在 pump 里**先试**（而不是等 `EPOLLOUT` 事件）：先 `send`/`sendfile`，
   只有拿到 `IO_WOULD_BLOCK` 才 `MOD` 成 `EPOLLOUT` —— 那一瞬间"写不动 → 又写得动"才是真正的状态变化，
   边沿一定会来。反面教材是"注册时就带上 `EPOLLOUT`"：那就是常驻，事件循环空转 100% CPU。
   实测（空闲 2 秒 0 jiffy、压测后 `Recv-Q = 0`）。
3. **`MOD` 只在"该关注什么"变了的时候发**：`conn.interest` 存的是**已注册的掩码**（不是第二份真相，
   方向仍由 `conn_next_step`/`conn_step_events` 决定）。省一次系统调用，也让"interest 有没有乱"可读。
4. **keep-alive 的规则进 `CONTRACTS.md` §1b，判断进 `http.c`**（`http_request_wants_close()`）：
   1.1 默认留、1.0 默认关、显式 `Connection` 覆盖默认。判断要读 `version` 与 `Connection` 的值，
   都是协议知识 —— 放进 `main.c` 就等于分层破了。
5. **`Connection` 的值按整体比较，不解析逗号列表**（记进 `docs/rfc/解析待定项.md` #13）：
   两个方向的误判都无害，而且是"已知简化"而不是"没想到"。
6. **声明了 `Content-Length > 0` 的请求一定不 keep-alive**（处理完走 DRAIN 收尾后关）：
   没被消费的 body 会被当成下一个请求解析，那是请求走私的经典入口（`解析待定项.md` #15）。
7. **状态机加一步 `CONN_STEP_NEXT_REQ`**：响应写完 + 要保留 → 先 `conn_reset_for_next_request()`
   （关 `file_fd`、放 `wbuf`、清 `rscan`/`drain`/`keep_alive`、回到 `CONN_READ_REQ`），再继续 pump。
   **流水线请求就靠这个衔接**：上一个 `read` 里顺手读进来的第二个请求还在 `rbuf` 里，
   所以 `step_read_req` 是"先扫缓冲、再 `read`"，不是"先 `read`"。
8. **accept 后设 `TCP_NODELAY`**（见下）。

## `TCP_NODELAY`：这一版最值钱的一行

一条响应是"先 `send` 头、再 `sendfile` 体"两次写；不关 Nagle 的话，第二段小数据要等客户端的
**延迟 ACK（约 40ms）**才发。实测同机同参数（`wrk -t2 -c100 -d10s`）：

| | Requests/sec | Latency(avg) |
|---|---|---|
| 不设 `TCP_NODELAY` | 2,233 | 44.50ms |
| 设了 `TCP_NODELAY` | **48,338** | **2.04ms** |

**18 倍**，而且是"什么都没优化、只加了一行 `setsockopt`"。原始数据与 `strace -c` 表：
`docs/bench/M2-ET验收.md`。

## 代价与已知取舍

- **没有 keep-alive 空闲超时**：挂着的空闲连接会一直占 fd，这是 **M3/T3.1 的题**（`last_active` 已经在了）。
  当前只在 DRAIN 阶段有 200ms 截止时间。
- **每请求一次 `realpath()`**：`strace -c` 里 `readlink` 占 37.8% 的时间，是 M4 第一个该动的点（路径缓存）。
- `EPOLLRDHUP` 没注册：FIN 一定让 socket 变成"可读的 EOF"→ `EPOLLIN` 照来；写侧的半关闭也不该打断响应。
- 每次 `accept` 都 `setsockopt(TCP_NODELAY)` + `fcntl(O_NONBLOCK)`，两条系统调用/连接 —— 连接级，无所谓。
- `EMFILE` 时只记日志、直接停手（不 sleep、不空转），靠下一次连接带来的边沿自愈。

## 验收（2026-10-14 实测）

| 项目 | 结果 |
|---|---|
| `make check` | 单测 **22** 条 + e2e **69** 条全绿，零告警 |
| `make e2e-asan` | 69/69 |
| 慢客户端（`nc` 连上不发） | 别的客户端照常 `200`（e2e M 组；`wchan = do_epoll_wait`） |
| `wrk -t2 -c100 -d10s` | **48,338 req/s**、延迟 2.04ms、Non-2xx = 0、超时 = 0 |
| `ss -tln` 压测后 | `Recv-Q = 0`（ET 下 accept 排空） |
| ASan 版 `wrk -t4 -c200 -d30s` | 133,975 请求不崩、fd 6 → 6、ASan 报错 0、`SIGTERM` exit 0 |
| keep-alive | 同连接连发 3 个 200；`curl a b` 出现 `Re-using existing connection`；流水线两条响应 |
| 传输中 `kill -9` 20 个客户端 | 服务端不退、日志无致命错误、fd 回落 46 → 6 |
| `strace -c` | `epoll_wait` 仅 15 次/10 秒、`accept` 102 次（keep-alive 的直接证据）、`poll` 0 次 |
| valgrind | `ERROR SUMMARY: 0 errors` + `All heap blocks were freed` |
