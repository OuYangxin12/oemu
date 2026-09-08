# 任务卡：M5 — initrd + busybox shell（终点门）

> 状态：设计就绪，为终点门预留。依据：`roadmap-linux-boot.md` M5 验收
> 门 = 完整复现 `docs/linux-minimal-qemu.md` 的验收输出。

## 范围

- 做什么：`-initrd` 加载与 `/chosen` 注册；stdin raw mode → PL011 RX；
  中断接线（timer PPI 27/30、PL011 SPI 33）；`docs/booting-linux.md`
  （defconfig diff、initrd 配方、`/init` 脚本）；`--smp N`（可选，
  单核 shell 达成后才碰）。
- 不做什么：JIT/性能（M6）；FP/SIMD（踩到才从 M6 拉回）。

## 验收命令与通过标准（终点门）

```sh
# QEMU oracle 先行（镜像与 initrd 就绪时）：
scripts/qemu-oracle.sh guest/build/Image \
  'BOOT OK' 'MINIMAL-BOOT-CHECK-PASSED' 'SHELL_ALIVE' --expect-exit 0 \
  --stdin 'echo SHELL_ALIVE' --stdin-delay 10
# oemu 侧：同一断言集，qemu-system-aarch64 换成 oemu boot
make boot-linux LINUX=... INITRD=...
```

- 通过标准（L3 四门全绿）：earlycon 有输出 → 内核日志推进 →
  `BOOT OK` → `SHELL_ALIVE`；`poweroff` → PSCI SYSTEM_OFF → exit 0。
- CI：以内核镜像存在为前提，缺失即跳过（roadmap 既定假设）。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| L3 门 | `tests/guest/boot-smoke.sh`（或 ctest 驱动脚本） | 三标记 + exit 0 断言；stdin 喂入时序与 QEMU 对齐（guest 起来后再喂） |
| 黑盒 | `tests/unit/test_initrd.cpp` | initrd 加载地址、`/chosen` 字段（linux,initrd-start/end） |
| 黑盒 | `tests/unit/test_serial_input.cpp` | stdin raw mode → RX 字节、流控/缓冲策略 |

## Oracle 来源

- [x] `docs/linux-minimal-qemu.md` 全部验收输出与喂入时序（sleep 10）
- [x] **QEMU 能、oemu 不能 ⇒ oemu bug**——本里程碑最高调试纪律
- [ ] 调试手段：串口日志落盘 diff；`--max-insns` 预算防挂死；必要时
      QEMU 指令 trace vs oemu 步进 trace 的 diff 定位首个分歧指令

## 不变量复核

- [ ] 全部四绿 + 新模块覆盖门槛
- [ ] all-idle 时宿主 `select()` 等待，不空转烧核

## 风险与回退

- 性能预期：纯解释器启动按分钟计，"能跑"是门；调试时可用
  `--max-insns` 预算 + 阶段门逐段逼近，而不是整段盲跑。
- 串口早到字节：喂入时序与 QEMU 脚本逐字对齐，避免脚本不可移植。

## 落地进度

### 第 1 轮：boot 期生成 virt 设备树 + `-initrd`（本提交）
M5 的头号依赖是 `/chosen/linux,initrd-start/end`，而本机无 `dtc`、外部 DTB
又被 gitignore。据此把路线图 M4a 一直推迟的「DTB 生成器」补上——`boot` 默认
用 `oemu_fdt` 现场生成 virt 树（无 `-dtb` 时），彻底摆脱外部 blob 与 `dtc`，
initrd 单元由此白送。

- 新模块 `src/dev/virt_dtb.c` + `include/oemu/virt_dtb.h`：照 `qemu -machine
  virt` 与 `docs/linux-minimal-qemu.md` 的实证树生成——root(#address-cells=2/
  #size-cells=2/compatible/model/interrupt-parent)、`/chosen`(bootargs +
  linux,initrd-start/end + stdout-path)、`/aliases`、PSCI(smc)、单 A53、A15-GIC
  (dist+cpuif)、arch timer(4×PPI)、fixed-clock 振荡器、PL011(SPI 33)。phandle
  显式赋(intc=1, osc=2)以免做 fixup pass。`oemu_fdt_init` 会开 root 节点，
  `finish` 要求 depth==0，故末尾须 `end_node` 关 root。
- `boot()`：无 `-dtb` → 生成树并写 bus（`-append` 写进 **bootargs**，修掉旧
  fixture 把命令行塞进内核根本不读的 `bootline` 的哑弹）；有 `-dtb` → 保持旧
  的读文件+就地补丁路径（fixture 冒烟测试不变）。新 `-initrd <cpio>`：装入
  RAM 四分之三处（避开内核文本/半路 DTB/顶栈，留 16 MiB headroom），越界即报错。
- 白盒测试 `tests/unit/test_virt_dtb.cpp`(12)：经 `oemu_fdt_internal_find`
  读回整棵树钉死形状（root/memory/chosen initrd 单元/psci/cpu/gic/timer PPI/
  pl011 SPI33/aliases/header 总长/小容量可恢复失败）。

门：make test / make asan **877/877**、clang preset 877/877、tidy exit 0、
format-check 仅剩 `bench/corpus/k_addsub.c` 预存漂移。实证：`boot -kernel
guest/build/Image`（**不给 -dtb**）用生成树一路到 `No working init found.`
终态，与外部 DTB 一致。

### 第 2 轮：PL011 中断改接 GIC SPI 33（本提交）

`boot_run` 每片把 UART 的实时电平刷进 GIC 第 33 行的 pending，vCPU 的 IRQ 线
只由 `oemu_gicv2_irq_level` 决定。此前 PL011 直接顶一根扁平 IRQ 线——驱动 `IAR`
会读回 1023(spurious) 而丢弃中断，字节永远送不进 tty。改接后（本提交）无 initrd
启动仍抵达 `No working init found.` 终态、门全绿（877/877）。PL011 的 RX 模型
（`oemu_pl011_inject`/`irq_level`、16 深 RX ring、MIS）M4a 就写好了，缺的只是
这根接线。下一轮：stdin→inject 泵 + guest initramfs/`/init`/getty，跑通
`Run /init`+三标记+poweroff→exit 0。

### 第 3 轮：`-serial stdio`(stdin→RX) + initramfs 配方 + generic timer（本提交）

- `boot_run` 加 stdin 泵：`-serial stdio` 时把 fd0 设非阻塞 raw，每片
  `read(0)`→`oemu_pl011_inject`（环满即丢，与 QEMU 早到字节丢弃一致）。默认
  不泵（现有 stdout/--serial=file 行为不变）。
- guest 侧：`tests/guest/init.c`（libc-free、~1.4 KB、原始 syscall 的极简
  PID1，能复现 oracle 喂的两行 `echo SHELL_ALIVE`/`poweroff -f`）+
  `scripts/build-linux-initramfs.sh`（cross gcc `-nostdlib` 编 + kernel
  `gen_init_cpio` 打包 `/init`/`/dev/console`）。产物 `guest/build/initramfs.cpio`
  gitignore，配方入库。glibc 静态 init 太大（600 KB）会拖死解释器，故手写极简版。
- generic timer：`include/oemu/gtimer.h`/`src/dev/gtimer.c` 纯谓词
  `oemu_gtimer_pending(counter,ctl,cval)`（counter 作参注入 = 可测缝，非墙钟）；
  `boot_run` 每片按虚拟 timer 比较器刷 GIC PPI 30 的 pending。`test_gtimer`(5) 钉
  禁用/屏蔽/到点/越点/回绕。

效果 + 下一个障碍：initrd 路径此前卡死在 `Unpacking initramfs…`（jiffies 冻结），
timer 一接上 jiffies 走动、内核越过该点——但随即在 `FAR=0x28` 数据异常处死循环
（scheduler 跑起来后的一个空指针，属**下一个**保真 bug，非回退：无 initrd 路径
仍干净 panic、冒烟与四门全绿 882/882）。

### 第 4 轮：定位 initramfs 普通文件 populate 崩溃（oracle 实证 oemu 保真 bug）

- 用 System.map 反解崩溃 PC：内核在 `chown_common+0x48`（`ldr x0,[x20,#0x28]`，
  x20=0）取一级翻译错，FAR=0x28。`chown_common` 以 **dentry->d_inode==NULL（负
  dentry）** 被调用。临时在 `oemu_exc_take` 加一次性首异常追踪（已回退）确认：
  全程**第一个**同步异常就是这条 data abort（非更早的 undefined/SError）。
- **判别实验**：① timer 关掉（PPI 改 63）仍崩 → timer 无罪（连线正确、保留）。
  ② dir-only initramfs 干净 panic、任何**普通文件**条目（含 0 字节、改名 foo）
  即崩 → 触发点是往 rootfs(tmpfs) populate 一个 regular file。③ **QEMU oracle**
  跑同一 Image+同一 foo.cpio：正常 unpack、走到默认 init 搜索后干净 panic——
  ⇒ 这是 **oemu 的保真 bug**，不是内核/initramfs 问题。
- 结论：initramfs 里普通文件的 create/lookup 在 oemu 上返回了负 dentry（QEMU
  上是正 dentry），下游 `init_chown`→`chown_common` 遂取空 inode。root cause 需
  oemu↔QEMU 执行流对齐（见待办），非单靠静态读能定位。四门仍 882/882（本提交
  未动可执行代码；timer/接线保留）。
- **追补（PC-ring + dentry 探针，均一次性、已回退）**：exec 内加每指令 PC 环形缓冲
  + 首异常时 dump，并就地经 memops 读回 dentry 字段。定论调用链：`do_name →
  filp_open(O_CREAT)`（返回**非** err 的 struct file）`→ vfs_fchown → chown_common`；
  探针读到 `mnt`/`dentry` 均有效、`d_inode(+0x30)==0x0`（**内存里真的为 0**，非
  oemu 读错值）——即 `filp_open(O_CREAT)` 在 oemu 上把一次 create 变成"返回成功但
  dentry 为负"，QEMU 上则是正 dentry。故是 **oemu 执行态分歧**（create 那一路某指令
  语义/标志位有差），非取数 bug、非 timer、非 initramfs。dentry 处 d_flags 亦见
  RCU/PARALLEL 位，像一份被留在负态的 dentry。

### 第 5 轮：create 其实执行了 → 缩小到"建后即被清零/野 file"

- 把 PC 环放大到 6000、在首异常处 dump 全环并符号化：崩溃前的窗口里出现
  `new_inode`、`d_alloc`/`__d_alloc`/`d_alloc_parallel`、`d_add`/`__d_add`、
  `inode_init_owner`——即 `filp_open(O_CREAT)` 的 create **确实跑了**（`d_add` 会把
  d_inode 置成新建 inode），可崩溃时 `d_inode` 却为 0。⇒ **不是"跳过 create"**，而是
  **建好后 d_inode 又被清成 0**（或 `filp_open` 返回的 `struct file` 是野值，其
  f_path.dentry 指向另一份负 dentry）。头号嫌疑改为：`struct file`/`struct dentry`
  从 slab 取到的对象未正确清零（`__d_alloc` 的 kzalloc 区、或 `alloc_file`）→ oemu 的
  memset/页清零某条指令（`DC ZVA`/`STP` 归零 / `UBFM` 变址）与真机有差；或 dentry 发布
  原子（`hlist_bl` 位锁 = LL/SC）被误判致 `d_add` 未真正落盘。
- 判别小结：dir-only 干净；任意 regular file（含 0 字节 / 改名 foo / 加 `dir .`）即崩；
  关 timer 仍崩；QEMU oracle 同 Image+同 cpio 正常。四门不受影响（本会话未改可执行代码）。

### 待办（后续轮）

- **root cause（已缩小到 filp_open(O_CREAT) 的 create 路径）**：做一次单步 oracle
  diff：oemu 侧加 `--trace`（反汇编每步 PC，已有 PC-ring 雏形）+ 在 `shmem_create`/
  `vfs_create`/`d_add` 边界断点；QEMU 侧 `qemu-system-aarch64 -singlestep
  -d in_asm,int` 跑同段。对齐到第一条分歧指令。重点排查：dentry 发布相关原子
  （`hlist_bl` 位锁 `test_and_set_bit`=LL/SC、`cmpxchg`）、`d_add`/`__d_add` 内联路径、
  `do_exclusive` 的 monitor（现仅 STXR 清、不在同址 plain store 时失效——先证伪/证真）。
- 修好后：喂 stdin（`-serial stdio`）取 `SHELL_ALIVE` + `poweroff`→exit 0。
- L3 门 `scripts/boot-linux-gate.sh`（缺镜像即跳过）+ `make boot-linux` target。
