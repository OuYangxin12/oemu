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

### 待办（后续轮）

- 中断接线：virtual timer PPI(27) 经可注入 clock 触发 + PL011 SPI(33) RX 中断
  → 喂进 GIC `set_pending`，否则交互 shell 收不到喂入的字节。
- stdin raw mode → PL011 RX，喂入时序与 oracle 对齐（guest 起来后再喂）。
- guest 侧 initramfs（`gen_init_cpio` 现成）+ 静态 busybox + `/init`（打印三
  标记后 `poweroff`）。镜像 gitignore，脚本入库。
- L3 门 `tests/guest/boot-smoke.sh`（缺镜像即跳过）+ `make boot-linux`。
