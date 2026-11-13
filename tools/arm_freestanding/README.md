# ARM freestanding 头（v0.9.1 B3）

**为什么需要它们**：clang 自带 ARM/Thumb-2 后端（`-target thumbv7m-none-eabi`），
但没有 ARM libc 的头文件。core 只依赖 6 个 libc 函数，所以提供这几个最小声明
就能**真的交叉编译出 Thumb-2 目标文件**，从而拿到接近真实的固件代码体积——
这比"用宿主 x86-64 近似"可信得多（x86-64 与 Thumb-2 指令密度差 1.5~2x）。

**这里声明的就是 core 依赖的全部 libc 面**（`tools/footprint.py --arm` 会校验）：

| 函数 | 用途 |
|---|---|
| `memcpy` / `memmove` / `memset` / `memcmp` | 缓冲与结构体操作 |
| `strlen` / `strcmp` / `strncmp` / `strchr` | 注册表里的名字比较 |

除此之外 core **不依赖**任何 libc：没有 malloc、没有 printf/snprintf
（`writer.c` 自带整数格式化）、没有文件/时间/线程 API。

**这些头不是拿来给真实固件链接的**——真实固件用你自己的 libc/newlib。
它们的唯一用途是让尺寸测量可以在这台没有 ARM SDK 的机器上跑起来。
