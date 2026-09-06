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
