<!-- markdownlint-disable MD033 MD036 MD041 -->

<div align="center">

# 😨 libschrodinger 🤣

**「我修复了 Linux 崩溃信息看得懂的 Bug」**

</div>

> [!WARNING]
> **该项目仅供以测试和娱乐为目的的使用。**
>
> 当前 `libschrodinger` 的实现面向 glibc/Linux（x86-64 为主，aarch64 尽力支持指令地址提取）。请勿将 `libschrodinger` 设置在 shell 的全局配置、systemd 环境或生产环境的 Linux 机器中，以免遇到无法诊断的问题。

---

## 📖 About

`libschrodinger` 是一个 Linux 上的 `LD_PRELOAD` 整活项目。

`libschrodinger` 只拦截致命信号（`SIGSEGV`、`SIGBUS`、`SIGILL`、`SIGFPE`、`SIGABRT`），在崩溃时弹出一个微软风格的简体中文错误对话框，还原某 **现代商业操作系统** 那个「指令引用的内存不能为 read」的神秘提示：

> `"0x00000000004011b7" 指令引用的 "0x0000000000000000" 内存。该内存不能为 read。`

普通 errno 报错、正常退出、`SIGINT`、`SIGTERM` 均不受影响。

## ✨ 行为

对话框上有两个按钮：

- **确定** — 恢复之前的信号处置并重新投递原信号，程序按正常路径终止（默认产生 core dump）。
- **取消** — 对话框立即关闭，随后在终端（`konsole`，回退到 `kitty`）中启动 `gdb -p <pid>` 附加到仍然冻结在信号处理上下文中的进程；关闭 gdb 后，程序才按原信号终止。

各信号使用的文案：

| 信号 | 文案模板 |
| --- | --- |
| `SIGSEGV`（`SEGV_MAPERR`） | `"<ip>" 指令引用的 "<addr>" 内存。该内存不能为 read。` |
| `SIGSEGV`（`SEGV_ACCERR`） | 同上，但为 `written` |
| `SIGBUS`（`BUS_ADRALN`） | 同上，但为 `aligned`（故意保留的破文案） |
| `SIGBUS`（其他） | 同上，`read` |
| `SIGILL` | `"<ip>" 指令引用的 "<addr>" 内存。该指令不能为 execute。` |
| `SIGFPE`（整数除零等） | `"<ip>" 处发生整数除法。该除数不能为 zero。` 等，按 `si_code` 细分 |
| `SIGABRT` | MSVCRT 风格：`Runtime Error!` 一段，仅一个 `确定` 按钮 |

`SIGFPE` 与 `SIGABRT` 不会伪造内存故障地址。

## 🚀 Build

需要 Nix（提供可复现的 Qt6 Widgets 开发环境）。克隆后编译：

```sh
gh repo clone LyCecilion/libschrodinger
cd libschrodinger
nix develop    # 或 direnv allow
make
```

编译产物：

```text
./build/libschrodinger.so    # ELF 共享对象（LD_PRELOAD 库）
./build/schrodinger-dialog   # Qt6 崩溃对话框辅助程序
```

可用 `file` 确认类型：

```sh
file ./build/libschrodinger.so ./build/schrodinger-dialog
```

`make clean` 仅删除 `build/`。Makefile 默认通过 `pkg-config` 消费 Nix 提供的 Qt6 元数据；等价的主机 Qt6（如 Fedora 的 `qt6-qtbase-devel`）也可作为 Nix 之外的回退，不改变运行时行为。

## 📝 Usage

对一次命令注入（不会修改当前 shell 的后续命令）：

```sh
LD_PRELOAD="$PWD/build/libschrodinger.so" <program>
```

一个可复现的崩溃示例：

```sh
LD_PRELOAD="$PWD/build/libschrodinger.so" \
  python3 -c 'import ctypes; ctypes.string_at(0)'
```

- 点击 **确定** → 正常终止 / core dump 行为。
- 点击 **取消** → 对话框先关闭，随后 `konsole`（或 `kitty`）打开 `gdb -p <pid>`，可检查仍存活的进程与栈；退出 gdb 后进程终止。

### 手动触发各信号

```sh
# SIGABRT
LD_PRELOAD="$PWD/build/libschrodinger.so" python3 -c 'import os; os.abort()'
# SIGFPE（整数除零）
LD_PRELOAD="$PWD/build/libschrodinger.so" python3 -c '1//0'
# SIGSEGV（空指针读）
LD_PRELOAD="$PWD/build/libschrodinger.so" python3 -c 'import ctypes; ctypes.string_at(0)'
```

## ❓ FAQ

- **目标平台**是 glibc/Linux。不支持 musl、BSD、macOS、Windows，也不支持静态链接程序。
- **setuid/setcap 程序**不注入 `LD_PRELOAD`，本库同样不注入。
- **尽力而为**：辅助程序或 gdb 无法启动、子进程失败时，一律回退为「恢复默认信号处置并重新投递原信号」的普通终止，绝不吞掉崩溃。需要图形环境（X11/Wayland）才能显示对话框。
- 替换了致命信号处置的程序可能不会弹出对话框（其自身处置优先）。
- 若 `konsole` 与 `kitty` 都不可用，`取消` 视为调试请求失败，直接正常终止。

## 🤔 原理

> **TL;DR: `LD_PRELOAD` 加载本库后，`__attribute__((constructor))` 在崩溃前缓存辅助程序路径、程序名与终端/gdb 路径，并为致命信号安装 `SA_SIGINFO` 处理器。崩溃时处理器 fork 出一个干净的 Qt 辅助进程显示对话框，按用户选择恢复并重投递原信号，或先附加 gdb 再终止。**

崩溃路径严格限制在 `fork`/`exec`/`wait`/`write`/`kill` 一族异步信号安全操作：所有非平凡字符串在构造函数中预计算；地址用固定缓冲区的十六进制格式化器生成；fork 使用绕过 atfork 处理器的裸 syscall（x86-64 用 `SYS_fork`，aarch64 用 `SYS_clone`），避免信号打断了持锁线程后子进程死锁；子进程在 `exec` 前 `unsetenv("LD_PRELOAD")`，使 Qt 辅助程序不会递归安装本处理器。

一个 `volatile sig_atomic_t` 重入守卫确保第二个致命信号到来时立即恢复默认处置并重投递。

## 📄 License

[MIT LICENSE](./LICENSE). By Limity'roChen & LyCecilion, 2026.

---

<div align="center">

🍀 | 🌌 | 🪼 | ❄️

</div>
