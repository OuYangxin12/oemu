# 实证基线：QEMU 上的最小 AArch64 Linux（busybox）

日期：2026-09-06。本文记录"最小 Linux"在 `qemu-system-aarch64 -machine virt`
上的完整构建与验收过程，作为 oemu 路线图（`docs/roadmap-linux-boot.md`）的
实证依据。**"能跑 Linux"从此由这个配置定义**：同样的 Image + initramfs +
命令行，将来在 oemu 上复现同一组验收标记即为达成目标。

## 验收结果

```
qemu_rc=0                     # poweroff → PSCI SYSTEM_OFF → qemu 退出码 0
Run /init as init process
BOOT OK
MINIMAL-BOOT-CHECK-PASSED     # /init 打印的验收标记
~ # echo SHELL_ALIVE
SHELL_ALIVE                   # 交互式 shell 正常应答
reboot: Power down
```

组件账本：

| 组件 | 版本/规格 | 体积 |
| --- | --- | --- |
| 内核 | linux-6.6.156 LTS，tinyconfig + 精选 fragment | Image **3.2 MB** |
| 用户态 | busybox 1.37.0 静态链接（cross glibc 2.39） | 2.2 MB |
| initramfs | 未压缩 newc cpio（gen_init_cpio 生成） | ~4 MB |
| hypervisor | qemu-system-aarch64 8.2.2，TCG | - |

内核关键事实（来自启动日志）：PSCI **v1.1，SMC conduit**（从 DT 探测）；
arch timer **62.5 MHz**（virt）；根中断处理 `gic_handle_irq`（GICv2）；
4K 页、**VA_BITS=39**（tinyconfig 默认）；184 MB/256 MB 可用；内核代码仅
1728K。TCG 上从上电到 shell 仅数秒。

## 对 oemu 有直接意义的观察

1. **PSCI 走 SMC**，与路线图 D4（拦截 SMC 模拟 EL3）完全一致；QEMU 同样如此。
2. **内核读 ID 寄存器做特性决策**：日志显示 `CPU features: detected: CRC32
   instructions` 与 `32-bit EL0 Support` —— 它读的是 `ID_AA64ISAR0` /
   `ID_AA64PFR0`。oemu M2a 钉死的 ID 值在系统模式下必须做一次一致性审计
   （不能宣传内核会去用的、我们没实现的东西；FP/SIMD=0 与无 FP 路径要闭环）。
3. **tinyconfig 用 39 位 VA / 4K granule** —— M3 的地址翻译正确性测试应覆盖
   39 与 48 位两种 TCR 配置。
4. **uart 输入的时序坑**：qemu 在 t=0 就把管道 stdin 灌进 PL011 FIFO，内核几秒
   后才使能 UART，期间的字节被丢弃。测试脚本必须在 guest 起来后再喂输入
   （见 boot-test.sh 的 `sleep 10`）。oemu 的串口模型实现时同样要决策早到
   字节是缓冲还是丢弃，并与 QEMU 行为对齐以免测试脚本不可移植。
5. **DC ZVA 语义**：busybox 跑起来了，但 glibc/musl 的 memset 可能使用
   `dc zva`（真实"写零"语义，绝非 no-op）。M3 必须实现它；这是 M3 范围里
   最容易漏的一条。

## 构建过程（无 root 宿主机）

宿主：Ubuntu 24.04 (WSL2)，`sudo` 被 no-new-privileges 禁用。所有产物在
`guest/`（已 gitignore，镜像不入库 —— 路线图既定假设）。

### 工具链（deb 本地解包，apt-get download 不需要 root）

```sh
# qemu：递归闭包下载 + 解包（90 个 deb）
apt-cache depends --recurse --no-recommends qemu-system-arm \
  | grep -E '^[a-z0-9]' | sort -u | xargs -n1 apt-get download
# 全部 dpkg-deb -x 到 guest/qemu-root，LD_LIBRARY_PATH 指向其 usr/lib/...
# 交叉工具链：binutils-aarch64-linux-gnu + gcc/cpp-13-aarch64-linux-gnu +
#   libgcc-13-dev/libc6-dev/libc6-arm64-cross + linux-libc-dev-arm64-cross
```

两个坑：
- 解包出的 `aarch64-linux-gnu-as` 依赖私有库 `libopcodes-2.42-arm64.so`，
  需要 `LD_LIBRARY_PATH=guest/toolchain/usr/lib/x86_64-linux-gnu`。
- Ubuntu 交叉 glibc 的链接脚本（libm.a 等）写了**绝对路径**
  `/usr/aarch64-linux-gnu/...`，本地前缀下必须加
  `--sysroot=$G/toolchain` 才能找到分卷静态库。

### 内核（linux-6.6.156）

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- tinyconfig
scripts/config \
  -e TTY -e PRINTK -e SERIAL_EARLYCON \
  -e SERIAL_AMBA_PL011 -e SERIAL_AMBA_PL011_CONSOLE \
  -e ARM_ARCH_TIMER -e ARM_GIC -e ARM_PSCI_FW \
  -e BLK_DEV_INITRD -e DEVTMPFS -e DEVTMPFS_MOUNT \
  -e BINFMT_ELF -e BINFMT_SCRIPT -e FUTEX -e POSIX_TIMERS \
  -e PROC_FS -e SYSFS -e MULTIUSER -e KALLSYMS
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j16 Image usr/gen_init_cpio
```

注：tinyconfig 会关掉 printk/TTY 等一切，上面的 fragment 是让它能在
virt 上输出并跑 initramfs 的最小集合。`KERNEL_MODE_NEON` 保持默认 y
（对 qemu 无影响；oemu 按 D6 应配 n 验证）。`-j32` 曾在 prepare 阶段
莫名挂掉一次，串行重跑即成功 —— 并行构建不稳时降核重试。

### busybox 1.37.0（静态）

```sh
make defconfig
# 1) tc.c 依赖 CBQ，新内核头已删除：关掉 CONFIG_TC
# 2) busybox 1.37 已知 bug：aarch64 也定义 __SHA__，误入 x86 SHA-NI 路径
#    —— 关掉 CONFIG_SHA1_HWACCEL / CONFIG_SHA256_HWACCEL
# 3) 链接：make CROSS_COMPILE=aarch64-linux-gnu- \
#          CC="aarch64-linux-gnu-gcc --sysroot=$G/toolchain"
#    （CROSS_COMPILE 必须保留，否则 strip 用成宿主机的）
```

### initramfs（不需要 root 的 mknod）

`usr/gen_init_cpio` 按描述文件生成 cpio，设备节点写在 spec 里而非宿主
文件系统上：`/dev/console c 5 1`、`/init`、`/bin/sh → /bin/busybox`，
外加 `/usr/{bin,sbin}` 目录（`busybox --install` 需要）与先
`mount -t proc proc /proc` 再 `--install`。脚本：
`guest/build/initramfs.list`、`guest/build/init.sh`。

### 启动与冒烟测试

```sh
( sleep 10; printf 'echo SHELL_ALIVE\npoweroff -f\n' ) | \
qemu-system-aarch64 -machine virt,virtualization=off,gic-version=2 \
  -cpu cortex-a53 -m 256M -nographic -no-reboot \
  -kernel arch/arm64/boot/Image -initrd initramfs.cpio \
  -append "console=ttyAMA0 earlycon panic=-1 rdinit=/init"
```

自动化脚本：`guest/build/boot-test.sh`（断言三个标记 + 退出码 0）。
本地 qemu 包装器：`guest/bin/qemu-aarch64`。
