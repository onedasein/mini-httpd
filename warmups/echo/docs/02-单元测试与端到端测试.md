# 02 · 单元测试与端到端测试（M0 热身）

> 这份笔记回答两个问题：**什么样的代码才「可以被测试」**，以及**测试该分几层、各层负责什么**。
> 读法：§1 坐标系 → §2 单测的机械前提（缝）→ §3 C 里怎么组织 → §5 可测性设计 → §8 防假绿 → §9 落地清单。
> 实例：`warmups/echo`（2026-10-08 完成分层），约定：根 `TASKS.md`「测试分层约定」。

---

## 1. 坐标系：先把几个词分清

| 词 | 含义 |
|---|---|
| 单元 unit | 被测的最小逻辑单位，通常是一个函数、一个类、一个小模块 |
| 测试用例 test case | 一个具体场景，比如「端口传 `abc` 应该报错」 |
| 测试文件 test file | 放测试代码的文件，比如 `test_parse.c` |
| 测试函数 | 文件里的一个函数，通常对应一个用例 |
| 测试运行器 | 自动执行所有测试并报告结果的程序（Unity 就是） |

组织原则一句话：

> **一个被测模块 → 一个测试文件；一个测试文件 → 多个测试函数。**

不是「一个用例一个文件」，也不是「脚本传参、由 bash 判断对错」。

### 三层测试的分工

| | 怎么跑 | 速度 | 隔离 | 挂了能定位到 |
|---|---|---|---|---|
| **单元测试** | 进程内直接调用被测函数并断言 | 毫秒级 | 最好 | **具体到用例名** |
| 集成测试 | 多个模块一起 | 秒级 | 中 | 到模块 |
| **端到端 E2E** | 起完整程序、走网络/命令行 | 秒级～十秒 | 最差 | 只能说「整机某处不对」 |

两者互补，**不是替代**：

- 单测能穷举边界（`cap=0/1/2/4`、端口 `0/1/65535/65536`），但不知道「服务器还起不起得来」；
- E2E 知道整机还活着，但只能试一两条路径。

echo 的实际规模：单元测试 **6 个测试函数 / 32 行表驱动用例**（毫秒级），E2E **1 条回显断言**（秒级）。

---

## 2. 单测的机械前提：先有「缝」（seam）

★ **关键认知：没有缝的代码，写不出单元测试。**

echo 的第一版是**单文件、除 `main` 外零个函数**：端口写死 `8080`、`argv` 只在签名里出现从没被读。
那种形态下唯一可用的测试手段就是 E2E —— 因为程序的唯一入口就是「跑起来、连它的端口」，
而 `main` 天生不可调用（一个可执行只能有一个 `main`，且它一进来就 `bind`/`listen` 卡在 `accept`）。

所以要能「进程内调用 + 断言」，必须满足两条硬约束：

### 约束 A：被测模块必须编成**不含 `main`** 的对象/库

链接器视角：一个可执行只能有一个 `main`。测试程序自带 `main`（Unity 的
`UNITY_BEGIN` / `RUN_TEST` / `UNITY_END` 那一套），所以它**不能**链接 `src/main.c`。

```make
SRC     := $(wildcard src/*.c)
# 被测模块 = 除 main 之外的所有源文件
LIB_SRC := $(filter-out src/main.c, $(SRC))
```

漏了这一句，链接时立刻 `multiple definition of 'main'`。
这也是「**第一件事永远是把逻辑从 `main` 里搬出来**」的真正理由——不是审美，是硬约束。

### 约束 B：被测函数失败要**返回错误码**，不要 `exit()`

`exit()` 会把整个测试进程带走：一条用例失败，其余用例全部不执行，报告也没有了。

```c
/* 好：可测 */
int parse_args(int argc, char* argv[], struct config* out);   /* 0 成功 / -1 失败 */

/* 坏：不可测（早期版本就是 perror + exit(1)） */
void parse_args_or_die(int argc, char* argv[]);
```

同理，**被测函数里不要做 I/O 输出**（`printf`/`perror` 会污染测试输出）——错误用返回码表达，
打印交给调用方。

---

## 3. C 里怎么组织

### 目录

```
warmups/echo/
├── src/                     ← 被测代码：能进这里的才「可测」
│   ├── parse.h / parse.c
│   ├── dump.h  / dump.c
│   └── main.c               ← 唯一 main，只负责串起来
└── tests/
    ├── unit/                ← 进程内、毫秒级
    │   ├── unity/           ← 第三方框架（vendored，MIT），版本见 .VERSION
    │   ├── test_parse.c     ← 一个模块 → 一个测试文件
    │   └── test_dump.c
    └── e2e/run.sh           ← 起进程、走网络
```

### 一个测试文件 → 一个可执行（Unity 范式）

每个 `test_*.c` 自带 `main`：

```c
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_port_table);
    RUN_TEST(test_parse_rejects_wrong_argc);
    return UNITY_END();          /* 全过返回 0，有失败返回非 0 */
}
```

三个好处：

1. **隔离**：一个文件里的用例崩了（或误 `exit`）不牵连别的文件；
2. **定位**：输出天生带「文件:行:函数:用例名」；
3. **扩展免费**：Makefile 用 `wildcard` 自动发现，新增测试文件不用改构建。

> 另一种风格是 Criterion / CMocka：所有测试编成**一个**可执行、由框架提供 `main`。
> 规模小时两者都行；Unity 的「一文件一可执行」胜在隔离和定位。

### 构建图与四个要点

```
build/debug/
├── server       ← src/main.c + parse.c + dump.c            （产品）
├── test_parse   ← tests/unit/test_parse.c + parse.c + dump.c + unity.o   ← 不含 main.c
├── test_dump    ← tests/unit/test_dump.c  + parse.c + dump.c + unity.o   ← 不含 main.c
└── unity.o
```

```make
# 1) 自动发现：一文件一可执行
TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(TEST_SRC:tests/unit/%.c=build/debug/%)

build/debug/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O0 -o $@ $< $(LIB_SRC) $(UNITY_DEBUG)
```

| 要点 | 为什么 |
|---|---|
| `filter-out src/main.c` | 见 §2 约束 A |
| 第三方源码**单独编、用 `-w`** | Unity 套上 `-Wconversion -Wshadow` 会刷屏；vendor 的代码你不维护，别用你的告警策略约束它 |
| 测试文件加 `-Wno-missing-prototypes` | 否则每个 `setUp`/`tearDown` 都告警 |
| 测试可执行落 `build/` | 被 `.gitignore` 覆盖，不脏仓库；`tests/` 里只放源码 |

**vendor 依赖要可复现**：拉固定的 tag（Unity `v2.7.0`），不要 master；版本记在
`tests/unit/unity/.VERSION`。另外 `raw.githubusercontent.com` 的 tag 路径**必须 `curl -L`**，
否则会下到一个 69 字节的 `Temporary Redirect` HTML——文件名和大小都像成功了，内容却是错误页。

---

## 4. 表驱动：一类逻辑，多组输入

同一个函数、一堆输入输出组合，用**数组 + 循环**，不要写十个几乎一样的测试函数：

```c
typedef struct {
    const char* name;       /* 用例名直接进失败信息 → 用 ASCII！ */
    const char* port;
    int         want_rc;
} ParseCase;

void test_parse_port_table(void) {
    static const ParseCase cases[] = {
        {"upper bound", "65535",  0},
        {"too big",     "65536", -1},
        {"not a number","abc",   -1},
        {"trailing junk","80abc",-1},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        const ParseCase* tc = &cases[i];
        char* argv[] = {(char*)"server", (char*)"127.0.0.1", (char*)tc->port, NULL};
        struct config c;
        TEST_ASSERT_EQUAL_INT_MESSAGE(tc->want_rc, parse_args(3, argv, &c), tc->name);
    }
}
```

echo 里两个函数共 **32 行用例**（`test_parse_port_table` 14 行、`test_format_bytes_table` 18 行）。

两个细节：

- **用例名会被打进失败信息**，所以它要能一眼看出场景。失败长这样：

  ```
  tests/unit/test_parse.c:48:test_parse_port_table:FAIL: Expected 0 Was -1:too big
  ```

  一行同时给出「文件:行、测试函数、期望/实际、**是哪条用例**」——这是 bash 脚本给不了的定位精度。

- **用例名用 ASCII**：Unity 的 `TEST_ASSERT_*_MESSAGE` 会把非 ASCII 转义成 `\xNN`
  （中文用例名会变成 `\xE4\xB8\x8B...`），中文写注释。

---

## 5. 可测性设计：纯函数留在中心，I/O 挤到边缘

对比两个 API 设计：

```c
/* 差：函数自己写 fd → 单测要造管道、读回来比对 */
void dump_bytes(int fd, const unsigned char* buf, size_t n);

/* 好：拆成「纯函数 + 薄壳」 */
size_t format_bytes(char* out, size_t cap, const unsigned char* buf, size_t n);  /* 纯：可穷举 */
void   dump_bytes(int fd, const char* tag, const unsigned char* buf, size_t n);  /* 壳：E2E 覆盖 */
```

`format_bytes` 无 I/O、无全局状态，喂输入断言输出即可；`dump_bytes` 只剩「把结果写到 fd」。
**能穷举的逻辑不要和 I/O 缠在一起。**

另一个非常适合单测的 API 形态是 **snprintf 语义**：

- 返回「**本该**写入的字符数」（不含结尾 `'\0'`）；
- 截断时只写 `cap-1` 个字符 + `'\0'`。

契约明确，边界就能成组断言（`cap = 0/1/2/4/5/8`）。
⚠ 判断是否截断必须拿**返回值**和 `cap` 比（`len >= cap`），**不能看「实际写了几个字节」**——
`snprintf` 的返回值是「本该写多少」，这是最容易写错的一处。

---

## 6. 测试在写规格：三条「跑出来才知道」

写 echo 的用例时有三次我的直觉是错的。**这三条都不是读代码读出来的，是跑出来的**：

1. **`strtol` 会跳过前导空白、接受 `+`/`-`**。
   所以 `" 80"` 被**接受**（原以为该拒），而 `"80 "` 被 `*end != '\0'` 挡住。
   现在两条都固化成用例，以后谁改 `parse_args` 都会立刻知道。
2. **`snprintf` 返回的是「本该写入的长度」**，不是「实际写了多少」（见 §5）。
3. **`cap == 0` 是「连 `'\0'` 都放不下」的边界**：必须一个字节都不写。
   用例用哨兵 `memset(out, '?')` + 断言 `out[0]` 仍是 `'?'` 来**证明「没发生」**。

第 3 条那种「证明没发生」的断言是单元测试独有的能力——E2E 观察不到内存里有没有被多写一个字节。

> 写测试的过程本质上是在**给函数补规格**。凡是「我以为是这样」的地方，都值得写成一条用例。

---

## 7. E2E 的位置与写法

echo 的 `tests/e2e/run.sh` 结构（也是这类脚本的通用骨架）：

```bash
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)   # ← 数清自己在第几层！
BIN=${1:-$ROOT/build/debug/server}                          # ← 收 $1，才能 debug/ASan 双跑
work=$(mktemp -d); trap 'cleanup' EXIT                      # ← 保证服务端一定被收掉
"$BIN" 127.0.0.1 $PORT >"$work/srv.log" 2>&1 & SRV=$!
sleep 0.3                                                   # ← 给 bind + listen 一点时间
got=$(printf 'hi\n' | timeout 10 nc -N 127.0.0.1 $PORT)
[ "$got" = "hi" ] && exit 0 || { cat "$work/srv.log"; exit 1; }
```

四个要点：

- **二进制路径当 `$1` 收** → 同一套断言能在 debug 与 ASan 上双跑（`make e2e` / `make e2e-asan`）。
- **`ROOT` 要数清层级**：脚本从 `tests/run.sh` 挪到 `tests/e2e/run.sh` 后，
  `$(dirname ...)/..` 必须改成 `/../..`，否则 `ROOT` 会指到 `tests/`——而且**脚本不会报错**，
  只会因为找不到二进制而失败，很容易误判。
- **`trap` + `mktemp -d`**：服务端是后台进程，不设 trap 就会残留、占住端口。
- **失败时把服务端日志打出来**（`cat "$work/srv.log"`）：E2E 唯一的诊断材料就是它。

> 本机 `nc` 是 OpenBSD 版，**必须 `-N`**：否则 stdin EOF 后两边互等，管道永久挂死。

---

## 8. 防假绿

★ 最危险的假绿是「**一个测试都没找到，却报全部通过**」。

```make
test: $(TEST_BIN)
	@if [ -z "$(TEST_BIN)" ]; then \
	  echo "没找到任何单元测试（tests/unit/test_*.c）"; exit 1; \
	fi; \
	rc=0; for t in $(TEST_BIN); do "$$t" || rc=1; done; exit $$rc
```

三条纪律：

1. 空套件**必须报错**，不许报「通过」；
2. 判定只看**退出码**，不要靠肉眼看输出（`make check; echo $?`）；
3. 失败信息必须带「文件:行:函数:用例名」，否则等于没有定位。

（同源的坑：本机的 bash 工具每次调用都是独立环境，`/tmp` 里上一步建的文件下一步会**静默消失**，
让「验证」假通过——临时文件一律放会话工作区。）

---

## 9. 落地清单：换一个 warmup 照抄

1. **先拆 `src/`，暂不碰测试**。
   把逻辑从 `main` 搬进 `模块.c/.h`；失败返回错误码、不 `exit`；能纯的函数做成纯函数。
   *验收：`make debug` 零告警，手工 `nc` 回显还正常。*
2. **引 Unity、写第一个测试文件，先手工链接一次**再写 Makefile 规则：
   ```bash
   gcc -std=c11 -Wall -Wextra -Isrc -Itests/unit/unity -g -O0 -o build/debug/test_parse \
       tests/unit/test_parse.c src/parse.c src/dump.c tests/unit/unity/unity.c
   ```
   ⚠ 这条命令里**绝不能出现 `src/main.c`**。
   *验收：这个可执行自己打出 PASS 并返回 0。*
3. **再写 Makefile 的三个目标**：`test`（单元）/ `e2e`（脚本）/ `check`，每个加 `-asan` 变体。
4. **把脚本回归挪到 `tests/e2e/`** 并修 `ROOT`；根 `Makefile` 加 `e2e` / `e2e-asan` / `check` 转发。
5. **更新文档**：本 warmup 的 `README.md` + 根 `TASKS.md` 的对应条目。

Makefile 骨架（可直接抄）：

```make
SRC_DIR  := src
SRC      := $(wildcard $(SRC_DIR)/*.c)
LIB_SRC  := $(filter-out $(SRC_DIR)/main.c, $(SRC))   # ← 缝

UNITY_DIR   := tests/unit/unity
UNITY_DEBUG := build/debug/unity.o

TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(TEST_SRC:tests/unit/%.c=build/debug/%)

$(UNITY_DEBUG): $(UNITY_DIR)/unity.c                  # 第三方：单独编、-w
	@mkdir -p $(@D)
	$(CC) -std=c11 -g -O0 -w -I$(UNITY_DIR) -c -o $@ $<

build/debug/test_%: tests/unit/test_%.c $(LIB_SRC) $(UNITY_DEBUG) Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) -Wno-missing-prototypes -I$(SRC_DIR) -I$(UNITY_DIR) \
	      -g -O0 -o $@ $< $(LIB_SRC) $(UNITY_DEBUG)

test: $(TEST_BIN)      # 见 §8 的空套件守卫
e2e: debug
	bash ./tests/e2e/run.sh ./$(DBG)
check: test e2e
```

---

## 10. 与 TASKS.md 的对应

| 内容 | 位置 |
|---|---|
| 分层约定（目标名、落地要点） | 根 `TASKS.md`「2. 测试分层约定」 |
| 已完成分层的实例 | `T0.3`（`warmups/echo`，Unity v2.7.0，6 测试函数 / 32 行用例） |
| **尚未分层**，等着照这套做 | `warmups/echo_pre`、`warmups/httpd/v1`（`http.c` 请求解析、`file.c` 路径校验是表驱动天堂）、`warmups/shell`（`parse.c` / `exec.c` / `builtin.c`） |
