# ADR-003 · 连接表：按 fd 索引的指针数组 + conn 独立模块

- **状态**：已定（2026-10-14，M2 阶段 A）
- **日期**：2026-10-14
- **相关**：`src/conn.h`、`src/conn.c`、`tests/unit/test_conn.c`、`CONTRACTS.md` §1（503 行）、`TASKS.md` T2.1a/b/c、T2.2a

## 背景

M2 要引入「连接对象」：每连接一份读缓冲、写缓冲、消费偏移、活跃时间。有两个岔路口：
① `fd → conn` 用什么数据结构；② 这段代码放哪个文件。

## 选项

| 决策点 | 选项 | 好处 | 代价 |
|---|---|---|---|
| 映射 | **A. 按 fd 索引的指针数组** `conn *g_by_fd[CONN_MAX_FD]` | O(1)、没有哈希/冲突代码；fd 本来就是小整数、天然索引 | 静态 4096 个指针（32KB）；fd ≥ 4096 拿不到槽位 |
|  | B. 哈希表 | 不受 fd 上限约束 | 要为「fd 是小整数」付哈希/冲突/迭代的代码 |
|  | C. 固定连接池 + 空闲链表 | 无 malloc、缓存友好（M4 想要的样子） | 现在就得定池大小；池满的行为和 fd 上限一样要处理 |
| 放置 | **A. `src/conn.{h,c}` 独立模块** | **单元测试能直接调它**（测试只链接 `LIB_SRC`，不含 `main.c`） | 多一对文件；DESIGN §2.2 的模块表要补一行 |
|  | B. 写在 `main.c` 里 | 少一个文件 | T2.1b/T2.1c/T2.2a 的验收（都要 `socketpair` 单测）**做不到** |

## 决定

1. 映射用 **A**：`static conn *g_by_fd[CONN_MAX_FD]`（`CONN_MAX_FD = 4096`）+ 每连接 `calloc`；
   拿不到槽位（fd 越界 / 同一个 fd 重复建 / `calloc` 失败）→ 回 **503** 后关掉。
2. 放置用 **A**：`src/conn.{h,c}`；IO 包装（`io_read` / `io_write` / `io_read_file` / `io_wait`）也放这里，因为它们服务于连接。
3. 连接对象**拥有 fd**：`conn_free()` 负责 `close(fd)`；`conn_free(NULL)` 安全。

## 后果

- `CONTRACTS.md` §1 多一行 **503**；`http_status_text()` 加了 `"Service Unavailable"`。
- **T2.5 的注意点**：`epoll_ctl(EPOLL_CTL_DEL)` 必须在 `conn_free()` **之前**调用 —— close 由它做，
  而 fd 号一旦被复用，就会踩到「已 DEL / 未 DEL」的坑。这条已写进 `conn.h` 的注释。
- M4 若要改成连接池：只动 `conn_alloc` / `conn_free` 两个函数，接口不变。
- ⏳ `DESIGN.md` §2.2 的模块表应补 `conn.c`（连接状态与 IO 包装）—— 本次**没改** DESIGN（设计真源归你）。
