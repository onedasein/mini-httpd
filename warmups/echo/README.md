# 目标
命令行启动，监听 IPv4 TCP 端口，循环 accept，串行处理连接，把收到的字节原样写回。

# 非目标
并发、IPv6、TLS、配置文件、日志系统、优雅关闭、性能优化、完整错误恢复。

# 源码结构
```
src/
├── parse.h / parse.c   参数解析：parse_args(argc, argv, &cfg)
├── dump.h  / dump.c    字节显形：format_bytes()（纯函数）+ dump_bytes()（I/O 薄壳）
└── main.c              唯一 main：解析参数 → socket/bind/listen → accept 串行回显 → close
```

拆成三个模块不是为了好看，是为了**能被单元测试直接调用**：被测逻辑必须从 `main` 里出来，
编成不含 `main` 的对象（`make` 里用 `filter-out src/main.c` 出库），否则链接测试程序会
duplicate symbol。原理见 `docs/02-单元测试与端到端测试.md`。

# 用法
```bash
make run                                # 默认 127.0.0.1 8080
make run ARGS="127.0.0.1 9090"          # 换参数
./build/debug/server 127.0.0.1 8800     # 直接跑

printf 'hi\n' | nc -N 127.0.0.1 8080    # 手工验回显
```

参数必须给全 `<ip> <port>`：`argc != 3` 报用法并退出码 1；端口范围 `1..65535`；
`ip` 用 `inet_pton` 校验，非法时报 `ip 不合法` 退出码 1。

> 本机 `nc` 是 OpenBSD 版，**必须 `-N`**：否则 stdin EOF 后两边互等，管道永久挂死。

# 测试
两层，命名与全仓库统一（约定见根 `TASKS.md`「测试分层约定」）：

| 目标 | 层级 | 跑什么 |
|---|---|---|
| `make test` | 单元测试，毫秒级 | `tests/unit/test_*.c`：进程内直接调 `parse_args` / `format_bytes` 并断言 |
| `make e2e` | 端到端，秒级 | `tests/e2e/run.sh`：起进程、`nc` 走网络、比对回显 |
| `make check` | 两层一起 | 提交前跑这个 |

每个目标都有 `-asan` 变体：`test-asan` / `e2e-asan`，发布前双跑。
新增一个 `tests/unit/test_xxx.c` 会自动变成一个新的测试可执行，不用改 `Makefile`。

# 验收
`make check` 退出码为 0（debug 与 ASan 各跑一遍）。

# 笔记
- `docs/01-网络编程概念图.md` —— socket 生命周期、短读短写、`SO_REUSEADDR` 为什么在这台机器上复现不出来
- `docs/02-单元测试与端到端测试.md` —— 这次分层的原理：缝、表驱动、纯函数与 I/O 边界、防假绿
- 踩过的坑与「修正」记在根 `TASKS.md` 的 T0.3
