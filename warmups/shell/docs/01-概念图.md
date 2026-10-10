# 01 · mini shell 概念

> **这张图是坐标系**：TASKS.md 的验收命令 `ls | wc -l > out.txt` 里出现的每个名词
> （token / `Command` / `cmds[]` / `pipe` / `fork` / `dup2` / `execvp` / `waitpid`）都能在图上找到位置。
>
> **读法**：§1 总体结构 → §2 解析结果 → §3 进程与 fd 的诞生顺序 → §4 数据流 → §5 fd 表 → §6 关键点 → §7 自查。
>
> **格式约定**：
>
> - 概念图 / 流程图 / 时序图 → **Mermaid** 代码块（语言标注 `mermaid`）。GitHub 网页、VSCode 的 Markdown
>   预览、Chirpy 博客（front matter 加 `mermaid: true`）都能直接渲染；同时它仍是纯文本，能 diff、能 review、能进版本管理。
> - 表格 → Markdown 表格；命令行与结构体 → 带语言标注的代码块（`bash`、`c`）。
> - 不再手绘 ASCII 框图：中文是双宽字符，用 `+` `-` `|` 拼的框永远对不齐，改一个字就得整块重排。

## 1. 总体结构

```mermaid
flowchart TD
    IN["用户输入一行<br/>ls | wc -l > out.txt"]
    PARSE["解析器 parse_line()<br/>按空白切 token"]
    NEW["遇到管道符：新建一条 Command"]
    REDIR["遇到重定向符：记录 outfile"]
    CMDS["Command cmds[]<br/>cmds[0].argv / cmds[0].outfile<br/>cmds[1].argv / cmds[1].outfile"]
    EXEC["执行器 execute_line()<br/>1. pipe() 创建管道<br/>2. fork() 每个命令<br/>3. 子进程 dup2 重定向<br/>4. execvp 执行命令<br/>5. 父进程 close<br/>6. waitpid 回收"]
    RESULT["验收：out.txt 的内容 == ls 的行数"]

    IN --> PARSE
    PARSE -->|遇到管道符| NEW
    PARSE -->|遇到重定向符| REDIR
    NEW --> CMDS
    REDIR --> CMDS
    CMDS --> EXEC
    EXEC --> RESULT
```

一次输入只走三个阶段：**切词 → 填 `cmds[]` → 逐个 fork/exec**。
`parse_line` 只认三种情况：普通 token（追加进当前命令的 `argv`）、`|`（开一条新命令）、`>`（记当前命令的 `outfile`）。

## 2. 验收命令的解析结果

```bash
ls | wc -l > out.txt
```

| 下标 | `argv` | `outfile` |
|---|---|---|
| `cmds[0]` | `["ls", NULL]` | `NULL` |
| `cmds[1]` | `["wc", "-l", NULL]` | `"out.txt"` |

`outfile` 属于**它前面那条命令**：`> out.txt` 写在 `wc -l` 后面，所以挂在 `cmds[1]` 上——
重定向的是 `wc` 的 stdout，不是 `ls` 的。

## 3. 进程与 fd 的诞生顺序

时序图按代码顺序画，但 fork 之后父子进程是**并行**往下走的，父子各持有一份 fd 表副本。

```mermaid
sequenceDiagram
    autonumber
    participant P as 父 shell
    participant C1 as 子进程1：ls
    participant C2 as 子进程2：wc -l
    participant K as 内核管道

    P->>P: pipe(p) — 内核里建一个管道，返回两个 fd
    Note over P: p[0] 读端、p[1] 写端，此刻父进程两个都持有
    P->>C1: fork()
    Note over C1: dup2(p[1], STDOUT_FILENO)<br/>close(p[0]) / close(p[1])<br/>execvp("ls", ["ls", NULL])
    P->>C2: fork()
    Note over C2: dup2(p[0], STDIN_FILENO)<br/>open("out.txt") 后 dup2(fd, STDOUT_FILENO)<br/>execvp("wc", ["wc", "-l", NULL])
    C1->>K: write() 到写端 p[1]
    K->>C2: read() 从读端 p[0] 拿到数据
    Note over K: ls 全部退出 → 写端无剩余持有人 → 读端读到 EOF → wc 才结束
    P->>P: close(p[0]) / close(p[1])
    P->>C1: waitpid()
    P->>C2: waitpid()
```

「父进程先 close 两个管道端」是必须的：父进程也持有 `p[1]`，不关掉它，即使 `ls` 退出，读端也等不到 EOF，`wc` 会一直挂着。

## 4. 数据流

```mermaid
flowchart LR
    A["ls 的 stdout"] --> B["管道写端 p[1]"]
    B --> C["内核管道"]
    C --> D["管道读端 p[0]"]
    D --> E["wc -l 的 stdin"]
    E --> F["wc -l 的 stdout"]
    F --> G["out.txt"]
```

## 5. 文件描述符视角

| 进程 | fd0 | fd1 | fd2 |
|---|---|---|---|
| `ls` 子进程 | 终端 | `p[1]` 写端 | 终端 |
| `wc` 子进程 | `p[0]` 读端 | `out.txt` | 终端 |
| 父 shell | 终端 | 终端 | 终端 |

## 6. 关键点

- `|` 的本质：前一个命令的 stdout 接到管道写端，后一个命令的 stdin 接到管道读端。
- `>` 的本质：在子进程里 `open()` 文件，然后 `dup2(fd, STDOUT_FILENO)`。
- `execvp` 会替换子进程映像，执行外部命令。
- 父进程必须关闭所有管道端，子进程也要关闭自己不用的那一端，否则读端可能等不到 EOF。
- `waitpid` 用来等待子进程结束并回收资源。

## 7. 自查

1. fork 之后，父进程和两个子进程各自持有哪些 fd 端？（对照 §5 的表）
2. 为什么 `wc -l` 必须等到 `ls` 结束、并且所有写端都被关闭，才会拿到 EOF？
3. 如果父进程忘了 `close(p[1])`，`ls | wc -l` 会停在哪一步？
4. 为什么 `> out.txt` 的重定向要放在子进程里做，而不是在解析阶段就 `open()`？
