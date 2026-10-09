# 目标
写一个mini shell，支持管道| 与重定向>，用fork + execvp + waitpid实现

# 验收
在所写shell中ls | wc -l > out.txt结果正确
