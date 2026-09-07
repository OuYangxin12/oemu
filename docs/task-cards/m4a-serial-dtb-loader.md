# 任务卡：M4a — PL011 + DTB + Image loader + boot 协议

> 状态：设计就绪，待 M3 合入后实现。依据：roadmap M4 节 +
> `roadmap-linux-boot.md` M4a 验收门（内核 earlycon "Booting Linux"）。

## 范围

- 做什么：`src/dev/pl011.c`（DR/FR/CR/IBRD/FBRD/LCR_H/IMSC/RIS/MIS/ICR；
  16 项预分配 TX 环；RX 注入钩子）；`src/fdt/fdt.c`（纯函数 FDT
  builder）；`src/kernel/image.c`（AArch64 Image 头：magic `ARM\x64`、
  text_offset、image_size、IPA-size 标志诚实拒绝）；`oemu boot` 完整
  参数面（`-kernel/-dtb/-m/-append/--smp/--serial`）。
- 不做什么：GIC/timer/PSCI（M4b）；RX 的宿主 stdin 接入（M5）；SMP
  调度（M5）。

## 验收命令与通过标准

```sh
ctest --test-dir build/debug -R 'Pl011|Fdt|Image|Boot'
make test && make asan
# L3 第一门（镜像存在时）：
scripts/build-guest.sh tests/guest/psci_off.S                                      # oracle 侧
scripts/qemu-oracle.sh build/guest/psci_off.bin PSCI-OK --expect-exit 0
# oemu 侧等价驱动（实现 test_boot_smoke 扩展时落地）
```

- 通过标准：真实内核 earlycon 输出 "Booting Linux"（镜像缺失 CI 干净
  跳过）；`psci_off` guest 在 QEMU 已预验证（见已就位资产）。

## 已就位资产

- `tests/guest/psci_off.S`——完整 AArch64 Image（含 Image 头），已在
  QEMU virt 上验证：EL1 进入、PL011 输出、**运行时探测 x0 传入的
  DTB**（实测 x0=0x48000000，method=hvc）并按内核协议先
  PSCI_VERSION 协商再 SYSTEM_OFF，QEMU 退出码 0。产物路径
  `build/guest/psci_off.bin`（`scripts/build-guest.sh` 生成）。
  它同时是 M4a Image loader 的**现成合法样本**与 M4b PSCI 的验收
  程序——一份资产钉两个里程碑。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| 黑盒 | `tests/unit/test_pl011.cpp` | 寄存器行为、TX 环满/空、RX 注入、IMSC/MIS 中断位；经 aspace MMIO 驱动 |
| 黑盒 | `tests/unit/test_image.cpp` | 字节级构造 Image 头（新 `image_builder.h`，elf_builder 模式）：magic、text_offset、标志拒绝矩阵 |
| 黑盒 | `tests/unit/test_fdt.cpp` | 结构块/字符串块/根节点正确性；期望值经独立 reader 或 dtc 对照 |
| 死亡 | `tests/unit/test_pl011_check.cpp`、`test_image_check.cpp` | NULL 契约 |
| fork | `tests/unit/test_cli.cpp`（扩展） | `oemu boot` 参数面、退出码契约 |

- 测试支撑新增：`tests/support/image_builder.h`；fdt 对照用的微型
  reader（若 dtc 不可用则跳过对照用例，`GTEST_SKIP` 写明缺什么）。

## Oracle 来源

- [x] QEMU virt PL011 行为；`psci_off` 实测输出
- [ ] 早到字节策略：与 QEMU 对齐后写成模块头注释（实证观察 4：内核
      使能 UART 前的字节被丢弃；测试喂入时序据此对齐）
- [ ] Image 头字段：Linux `Documentation/arch/arm64/booting.rst`
      （本地树有）

## 不变量复核

- [ ] 设备回调零分配；TX 环 init 预分配
- [ ] 未实现 MMIO 寄存器行为成文（读返 0 / 写忽略 / Data Abort，逐个注明）
- [ ] loader 拒绝路径全部返回 `oemu_status`，不留半加载状态

## 风险与回退

- DTB 与 ID 寄存器、机器布局三者不自洽 → Linux 误判特性：fdt 测试
  与 ID 审计（M4b 内）交叉核对 `/cpus`、`/psci` 节点值。
- earlycon 不出字节的排障顺序：boot 协议（x0=DTB PA）→ UART MMIO →
  早期异常向量；L3 门天然把这三者拆成可独立排查的链路。
