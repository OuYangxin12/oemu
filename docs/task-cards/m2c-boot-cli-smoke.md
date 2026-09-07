# 任务卡：M2c 收尾 — `oemu boot` CLI + guest 冒烟

> 状态：已实现并本地全量验证（735/735 双绿 + tidy 通过 + PR 文件 format
> 干净，数字见完工记录）；guest 真身验证待工具链/CI（见遗留）。依据：
> `docs/verification-strategy.md`
> §M2c 收尾、`docs/roadmap-full-system.md` M2c "Accepts" 节、
> `docs/roadmap-linux-boot.md` §5（M2c 进行中 → 本卡收尾）。本卡同时是
> `docs/task-cards/` 里 M2c 缺失的那张卡。

## 范围

- 做什么：
  - `src/main.c` 增加 `boot` 子命令：`oemu boot -kernel <raw-bin>
    [--entry ADDR] [--max-insns N]`。机器布局对齐 QEMU virt oracle 基线
    （`linux-minimal-qemu.md`、两枚已实测 guest 的约定）：RAM 256 MiB 住
    `0x40000000`，raw image 逐字节拷入 `0x40080000`，EL1 + identity 进入
    （`SCTLR_EL1.M` 复位为 0，M3a 翻译层自动走 identity 分支），x0–x3 全零
    （M2c 无 DTB，x0=DTB 是 M4a 的事，写进注释防误解）。默认入口
    `0x40080000`（AArch64 Image 头 `b _start` 从首字生效）；`--entry`
    覆盖。
  - fake UART（明示的临时设备，M4a PL011 之前）：住 `0x09000000`
    （QEMU virt UART0 同址——同一枚 guest 两边可跑，oracle 可移植性是
    选址理由）。契约：写 DR(offset 0x00) 任意宽度 → 低字节落 stdout 并
    立即 `fflush`；读 FR(0x18) 返回 0（TX 不满不忙）；其余读返回 0、其余
    写忽略。设备回调零分配。
  - **退出协议（M2c 定版，写进 main.c 头注释）**：guest 打完 marker 后向
    DR 写 sentinel 字节 `0x04`（EOT）→ fake UART 回调
    `oemu_machine_poweroff(machine, 0)`（machine 的 sticky 事件正是为此
    设计）→ boot 循环的 `env->halted`（= 事件非 NONE，vcpu_run 逐条检查）
    在下条指令边界看到 → 退出码 = `machine->exit_code` = 0。随后 guest 若
    还有 `brk`，其向量表 handler `b .` park——那是 QEMU oracle 侧的收场
    方式（marker 已上线，脚本超时回收，与 `mmu_smoke` 同一约定）。
  - 新增 `tests/guest/el1_smoke.S`（roadmap-full-system 原文规格）：EL1
    启动自证（打 `"EL1"`）→ 置 VBAR_EL1 → `svc #0` 陷入并 ERET 返回 →
    fake UART 打 `"BOOT-OK"` → 写 EOT → `brk #0`。向量表 sync 组按
    ESR_EL1.EC 分诊：SVC(0x15) 前进 ELR 4 字节再 ERET；BRK(0x30，ARM ARM
    D17.2 breakpoint-from-same-EL) park；
    其余打 `"F"` park（失败必须能自报家门）。常量逐条注明 ARM ARM 出处。
  - 新增 `tests/unit/test_boot_smoke.cpp`（标签 `guest`）：经
    `/proc/self/exe` 的兄弟路径 `../../guest/el1_smoke.bin` 定位产物；
    不存在 → `GTEST_SKIP` 写明重建命令（`scripts/build-guest.sh
    tests/guest/el1_smoke.S`）。存在则 fork CLI
    `boot -kernel <bin> --max-insns 20000000`，断言 exit 0 且 stdout 含
    `EL1` 与 `BOOT-OK`（管道捕获，不窥探内部）。
  - `tests/unit/test_cli.cpp` 扩展：boot 子命令的 fork 式用例，镜像用
    **测试内字节构造的 raw bin**（GAS 已验指令字，零交叉工具链依赖，
    本地即可全量验证 CLI 行为面）。
- 不做什么：PL011/DTB/Image 头解析（M4a）；PSCI/poweroff conduit（M4b）；
  `--initrd`/`-dtb`/`-m`/`--smp`/`--serial` 参数面（M4a）；IRQ/GIC（M4b）；
  `src/` 库代码零改动（boot 全部长在 `main.c`）；**`oemu run` 行为字节级
  不变**（roadmap M2c Accepts 原文）。

## 验收命令与通过标准（动工前先写死）

```sh
cmake --build build/debug
ctest --test-dir build/debug -R 'Cli'            # boot CLI 用例全绿（本地可全验）
ctest --test-dir build/debug -L guest            # 无工具链=干净 SKIP，有=真跑
scripts/build-guest.sh tests/guest/el1_smoke.S   # 有 clang+ld.lld 时
scripts/qemu-oracle.sh build/guest/el1_smoke.bin EL1 BOOT-OK   # oracle 预验证
make test && make asan                           # 四绿底线，用例只增不减（≥721）
make format-check && make tidy
make coverage-summary                            # main.c 较 84% 基线只升不降
```

- 通过标准：本地无工具链环境下——Cli 用例全绿、guest 标签干净 SKIP、四绿
  全达标；`oemu run` 既有用例零改动零失败。有工具链/CI——guest 真跑 exit 0，
  QEMU oracle 预验证通过。

## 新增测试清单

| 层/风格 | 文件 | 钉住的行为 |
| --- | --- | --- |
| fork/黑盒 | `tests/unit/test_cli.cpp`（扩展） | boot 契约全表：默认入口即执行 0x40080000；`--entry` 覆写生效（park 镜像 + 覆入口 → exit 0，缺省 → BLOCKED 4）；UART 透传字节级精确（stdout == 写入序列）；EOT sentinel → exit 0；`--max-insns 0` → exit 3；缺文件 → 1；缺 `-kernel`/未知选项 → 2 |
| guest/L2 | `tests/guest/el1_smoke.S` + `tests/unit/test_boot_smoke.cpp` | 完整 boot 语义链：EL1 进入 → SVC 陷入/返回（exc 模块语义经真镜像验证）→ UART magic → EOT → exit 0；标签 `guest`，无产物/无工具链干净 SKIP |

- `tests/CMakeLists.txt` 注册项与标签：`oemu_add_test(test_boot_smoke ...
  LABELS guest)` + `add_dependencies(test_boot_smoke oemu_cli)`，并入
  `check` 依赖列表。
- 测试支撑新增：无（raw bin 构造小到一个测试内 helper，暂不进 support/
  ——第二处用到时再提取，避免过早抽象）。

## Oracle 来源（期望值从哪来，P2）

- [x] `docs/roadmap-full-system.md` M2c 原文规格（机器布局、guest 行为、
      Accepts 清单——范围与本卡一一对应）
- [x] GAS/clang 已验指令字（`test_vcpu.cpp` 既有语料直接复用：
      `str x0,[x1]`=0xF9000020 等；新字动工后经汇编器核对再写死）
- [x] QEMU virt oracle：`scripts/qemu-oracle.sh` 跑 el1_smoke 断言双标记
      （本机无 qemu → exit 3 语义干净跳过，CI 补位）
- [ ] ARM ARM：向量表布局/EC 编码条目号在 `el1_smoke.S` 注释内逐条引用
      （动工时补条目号，不引用不写死）
- 反例自查：boot 循环、fake UART、exit 协议三者都不给自己出题——
      CLI 用例的期望值全部来自任务卡上先写死的数字。

## 不变量复核（P4，合入前打勾）

- [x] 纯 C11；`main.c` 新增分配全经 buffer/machine 既有 seam；设备回调零分配
      （`boot_uart_read/write` 无任何分配路径）
- [x] 精确异常：el1_smoke 的 SVC→ERET 往返即 guest 级回归（`test_cli` 的
      boot 用例本地以真指令验证了陷入/返回/退出链）
- [x] 一切分配经 `oemu_allocator` seam（RAM 大块亦经 machine→aspace→seam）
- [x] `make test` / `make asan` / `make tidy` 全绿；`make format-check`
      对本卡文件干净（残留违规仅 `bench/corpus`，版本漂移产物，见完工记录）
- [x] 新代码行覆盖：`main.c` 85%（210/246）≥ 84% 基线，只升不降；豁免见完工记录
- [x] 失败不留痕迹：boot 参数错误在任何状态提交前返回；DECODE/UNSUPPORTED
      文案二分不受影响（P5）
- [x] `oemu run` 字节级不变（既有用例零改动、零失败）

## 风险与回退

- **fake UART 写成既定行为的坑**（策略文档点名的陷阱）：全部契约注释以
  "stopgap, M4a's PL011 replaces this" 定性；退出协议 sentinel 与设备
  分离——M4a 换真 PL011 时 sentinel 语义移到 poweroff 设备，guest 不改。
- **`oemu run` 回归**：boot 与 run 只共享 `read_file`/usage/parse helper；
  为 boot 引入的任何共享行都必须先过 run 既有用例。
- 本地无交叉工具链：el1_smoke 真身验证落在 CI/有工具链环境；风险由
  test_cli 的字节构造用例兜底——CLI 行为面本地全覆盖。
- 回退方案：boot 全部代码在 `main.c` 的独立 static 区，整块删除即回到
  M2c 前状态，零库面影响。

## 完工记录（P6：只写实际跑过的）

- 实际运行的配置与结果（真实数字，2026-07-02 本机）：
  - `make test`：**735/735，100% 通过**（M2c 前为 721；新增 `CliBootTest`
    boot 契约 13 例 + `BootSmoke` 1 例）。
  - `make asan`：**735/735，100% 通过**（同一套件过 ASan+UBSan）。
  - `ctest -R Cli`：22/22（本卡新增 13 例全在其中，含 stdout 管道捕获的
    UART 字节级断言）。
  - `ctest -L guest`：1 例干净 SKIP（无 clang+ld.lld，SKIP 消息给出重建
      命令——设计内结局，非失败）。
  - `make coverage-summary`：`main.c` **85%**（210/246，基线 84% 之上）；
    全库 TOTAL 95%。**豁免**：未覆盖 36 行为跨进程 CLI 无法注入
    allocator 的 init/OOM 失败分支（与既有 `run` 同类结构性豁免）及
    RESET 分支（PSCI 前无触发路径，M4b 随 `test_psci` 覆盖）。
  - `make tidy`：**通过（exit 0，零 error）**。
  - `make format-check`：本 PR 全部文件经 `make format` 规范化后**干净**
    （commit `style: clang-format the M2c additions`）。唯一残留违规在
    `bench/corpus/k_addsub.c`——本卡未触碰该文件，且 master 同树在 CI
    （ubuntu-24.04，clang-format 18）历史上 lint 全绿，本机为 Ubuntu
    25.10 / clang-format 21：**版本漂移产物，非本卡引入**。
- 遗留问题 / 跟进项：
  1. **lint 版本漂移**：CI 钉 ubuntu-24.04（LLVM 18），本地滚动到 LLVM
     21，对 `bench/corpus` 与个别对齐规则给出不同判定；跟进方向是 CI
     显式钉版本（如 `clang-format-18`）或统一 runner 基线，属 CI 工作面
     （`oemu-ci-workflow`），不在本卡范围。
  2. el1_smoke 真身 + QEMU oracle 预验证：本机只装了 clang-format/
     clang-tidy，无全量 clang + lld + qemu；CI `guest` job 按 M4b 既定
     计划落地时一并跑。
  3. `docs/linux-minimal-qemu.md` 基线的喂入时序到 M5 才需要；
  4. CI `guest` job（装 `gcc-aarch64-linux-gnu` 或 clang+lld）另行落地。
