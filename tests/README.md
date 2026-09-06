# oemu 测试套件

oemu 的全部测试都在本目录：以 GoogleTest 为骨架、经 CTest 驱动，每个模块一个
测试可执行文件。用例数量随开发持续增长，实时数目以
`ctest --test-dir build/debug -N` 为准。

设计一句话概括：每个模块的公共 API 通过 `include/oemu/` 做黑盒测试；纯内部决策
逻辑（对外不可见、错了也不吭声的那类）通过 `src/<module>/<module>_internal.h`
做白盒测试；abort 契约用死亡测试在独立文件中验证——全程由 allocator seam 提供
泄漏检测与内存耗尽注入。

只有测试翻译单元是 C++17（生产代码保持纯 C11），测试经公共头文件里的
`OEMU_BEGIN_DECLS` / `OEMU_END_DECLS` 与 C 库链接。这个拆分正是那对宏存在的
理由：少了 `extern "C"` 保护，最先炸掉的就是这里的链接。

> 每个测试文件具体钉住了哪些行为，以该文件**头部的注释**为准。文件级细节有意
> 维护在离代码最近的地方，本文只写不随用例增减变化的结构与约定。

## 目录结构

```
tests/
├── CMakeLists.txt        # 整个测试骨架：oemu_add_test()、标签、check 目标
├── support/              # 仅测试使用的共享基础设施
│   ├── tracking_allocator.h/.cpp   # TrackingAllocator 与 FailingAllocator
│   └── elf_builder.h               # 逐字节构造 AArch64 ELF64 镜像
└── unit/                 # 按关注点一文件，文件名即所测内容
```

`tests/CMakeLists.txt` 是注册测试的唯一入口，`oemu_add_test()` 封装了
`add_executable` + `gtest_discover_tests`：

```cmake
oemu_add_test(test_mymodule
  SOURCES unit/test_mymodule.cpp
  LABELS unit              # 或 "unit;whitebox" / "unit;death"
  [INTERNAL]               # 允许访问 src/<module>/<module>_internal.h
)
```

`gtest_discover_tests` 以 `PRE_TEST` 模式把每个 `TEST()` 注册为独立 CTest 用例，
失败精确到用例名，`-R` 可单独选中。每用例 60 秒超时。每个测试二进制都链接
`oemu`、`oemu_test_support`、GTest 与 `oemu_compiler_flags`——最后一项让
sanitizer 与 coverage 预设同样作用于测试代码。

## 运行方式

```sh
make test        # 配置 + 构建 + 全量运行（debug）
make test-fast   # 跳过较慢的死亡测试
make test-death  # 只跑死亡测试
make retest      # 只重跑上次失败的用例
make asan        # 同一套件在 ASan + UBSan 下
```

筛选就是普通 CTest（需要 gtest 自身参数时直接调二进制）：

```sh
ctest --test-dir build/debug -R Buffer          # 按名字正则
ctest --test-dir build/debug -L whitebox        # 按标签
ctest --test-dir build/debug -LE death          # 排除死亡测试
ctest --test-dir build/debug --rerun-failed
./build/debug/bin/test_buffer --gtest_filter='BufferOom.*'
```

### 标签

共三个标签：`unit`（全部）、`whitebox`（触达 internal 头文件的测试）、`death`
（fork 式 abort 测试）。它们以**单个合成标签**（`unit.death`）存储，而不是列表
——`gtest_discover_tests` 会把 `PROPERTIES` 参数摊平成分号列表，真正的多值
`LABELS` 属性传不过去。由于 `ctest -L` 按正则匹配，`-L unit`、`-L death`、
`-LE death` 的行为都符合预期。不要把它"修"成真正的列表——第一个之后的标签会被
静默丢弃。

### 影响测试的环境事项

- **TSan** 下的测试二进制经 `setarch -R` 启动（Linux 6.x 的 ASLR 会让 TSan 直接
  abort）；启动器挂在 `CROSSCOMPILING_EMULATOR` 上，discovery 阶段同样走它。
  本目录的代码无需关心。
- **GoogleTest 用系统包。** configure 会打印实际选中的是哪一个；conda 前缀里的
  陈旧副本是链接报错的常见原因。`CMakeLists.txt` 底部留有一份注释掉的
  FetchContent 备选。

## 三种测试风格

**黑盒**——默认形态。只走公共 API，对状态码与可观测状态做断言。注册时不加
`INTERNAL`，标签 `unit`。

**白盒**——用于纯内部逻辑，那些从外部看沉默无形的 bug：精确边界上的检查算术、
位布局、表格的良构性。模块把私有面发布在自己的私有头文件
（`src/<module>/<module>_internal.h`）里，只被模块本身和这些测试消费；测试目标
加 `INTERNAL`，include 写作 `"module/module_internal.h"`。这是纯 C 项目里让内部
逻辑可测的机制——相比把它标成 `static`（测试够不着）或让测试 `#include` 一个
`.c` 文件（脆弱）。专职白盒的文件标签为 `unit;whitebox`；`test_memory`、
`test_aspace`、`test_memops` 则在单个文件里混合公共 API 与内部辅助用例。

**死亡测试**——`OEMU_REQUIRE` 的 abort 契约（NULL 状态、非法寄存器号、EL2
启动）。abort 只有在 fork 出的子进程里才可观察，所以它们住在各自的
`test_*_check.cpp`，标签 `unit;death`，fixture 里设定 `threadsafe` 死亡测试
风格：ASan 下默认的 `fast` 风格会复用进程镜像、从被中止的子进程里报假泄漏，
`threadsafe` 改为重新 exec。套件名以 `DeathTest` 结尾，让 gtest 把它们排在
多线程套件之前。

## 测试支撑库（`tests/support/`）

**`tracking_allocator.{h,cpp}`** 接在 `oemu_allocator` seam 上（gmock 拦截不了
纯 C——没有虚函数可 mock）。两个 RAII 替身：

- `TrackingAllocator`——构造时安装、析构时恢复前一个 allocator，测试失败也不会
  把替身泄漏给下一个用例。统计 alloc/realloc/free 次数与未归还块数。
- `FailingAllocator`——让第 `fail_on_call` 次（1 起计数）分配失败；free 永远
  成功，注入失败后测试仍能清理。失败的 realloc 返回 NULL 且不碰原块，这正是库
  所依赖的 realloc 语义。

**`elf_builder.h`** 逐字节拼出真实的 AArch64 ELF64 镜像。加载器拒绝一切不严格
是静态 `ET_EXEC` AArch64 的东西，伪造结构体什么都测不到；字节级构造意味着失败
会精确点名肇事的头部字段，汇编器永远不会产出的畸形用例写起来与合法用例一样
容易，宿主机上也不需要 AArch64 链接器。它刻意独立于库去镜像磁盘格式——一个
include 了加载器自定义义的构造器，可能掩护一个"自己和自己一致"的加载器。
`test_elf` 与 `test_cli` 共用它，因为复制两份镜像构造器迟早悄悄不一致。

## 贯穿套件的设计技法

- **拆栈即查漏。** 多数 fixture 持有一个 `TrackingAllocator`，并在 `TearDown()`
  断言 `live_blocks() == 0`——fixture 里每个用例免费继承这条泄漏检查。teardown
  处报的失败通常意味着某个早退 `return` 漏了 dispose，或失败的 realloc 丢了原
  指针。有些 fixture 更进一步：`test_regs` 断言 `alloc_count() == 0`（寄存器
  状态是定长值类型），exec fixture 在 `SetUp` 记下 `alloc_count()` 并要求 run
  之后不变（步进循环必须零分配）。
- **在精确调用点注入 OOM。** `FailingAllocator` 按次计数，OOM 测试把失败钉在
  具体那次分配上（region 表、RAM 块、加载器的临时数组、某个 backing block）。
  推论：模块**缓存它 init 时的 allocator**，所以替身必须在 `*_init` 之前装好，
  且模块要在失败作用域之内 dispose。
- **失败的操作不留痕迹。** 库先全量校验再提交（没有 unmap 可用来善后），测试
  也按这条标准要求它：被拒绝的 map 不改 `region_count`，`write_bytes` 全有或
  全无，出错的指令什么也不提交——含 writeback 与 PC，失败的 decode 把输出
  清零。
- **独立参照物。** 期望值来自实现之外：decode 金标准语料由 llvm-mc 汇编产出，
  sysreg 编码从 clang 集成汇编器收割，异常相关值由经 QEMU 与 Linux 核对过的
  常量拼出，ELF 构造器自己镜像格式。拿代码核对作者自己想法的测试证明不了
  什么。
- **对纯逻辑穷举而非抽样。** 函数纯且输入空间够小时就枚举：条件码真值表
  （16 条件 × 16 旗标组合）、DecodeBitMasks 的整个编码空间、AArch64 模式校验的
  32 项判定表、20 万伪随机指令字的 decode 全应答扫描。
- **跨 seam 的 parity。** 重构引入间接层时（memops 总线 seam、exec 的包装
  入口），至少一个测试用两条路径跑同一负载并要求结果一致，包装层因此长不出
  自己的行为。

## 约定与常见坑

- 套件命名跟随模块关注点（`BufferOom`、`DecodeLoadStore`、`AspaceInternal`…）；
  死亡测试套件必须以 `DeathTest` 结尾。
- allocator 替身只经 RAII 构造安装。不用 RAII 安装的测试会把替身泄漏给下一个
  用例——典型的"单跑通过、全跑失败"。
- 测试代码是 C++：不能用指定初始化器（此处算 C 扩展——逐字段赋值，参照
  `test_aspace`）；`-Wconversion` 下小心枚举强转（取"最后一个枚举值再 +1"，
  不要取远超范围的值）。
- `oemu_sysreg_read` 等带 `warn_unused_result`；GCC 下裸 `(void)` 强转不够——
  把状态吃进一个弃置局部变量。
- `test_cli` 经 `/proc/self/exe` 定位 `oemu` 二进制（同 `bin/` 下的兄弟），
  找不到就 `GTEST_SKIP`，纯库构建不会误报。
- `check` 自定义目标先构建全部测试二进制再跑 CTest。新增目标时，记得把目标加进
  `tests/CMakeLists.txt` 里 `check` 的 `add_dependencies` 列表。

## 新增测试

1. 先选风格：公共 API → 并入该模块现有文件的黑盒部分；纯内部逻辑 →
   `test_<module>_internal.cpp` 加 `INTERNAL`；abort 契约 →
   `test_<module>_check.cpp` 加 `LABELS "unit;death"`。
2. 在 `tests/CMakeLists.txt` 用 `oemu_add_test(...)` 注册，并把目标加进 `check`
   的依赖列表。
3. 模块若会分配，继承（或编写）一个 `TearDown` 断言无泄漏的 fixture——若它
   根本不许分配，就再断言 `alloc_count == 0`。
4. 在文件头部的注释里说明该文件钉住的设计意图与边界（细节写在这里，而不是任何
   索引文档）；新增内部辅助函数优先穷举边界而非抽样；新增指令就把指令字连同
   llvm-mc 打印的汇编文本加进金标准语料。
5. 宣称完成前：`make test` 与 `make asan` 都必须 100% 通过（见顶层 `AGENTS.md`）。
