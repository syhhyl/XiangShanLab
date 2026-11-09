# XiangShan Issue #6343：AtomicsUnit 的 `hardwareError` 跨原子操作残留

> **基线**：昆明湖 V3，`c4cfd2b78a755e7157e639559c24fb42d3aeebda`（2026-08-11）。
> **修复**：PR #6316，merge commit `cbf2d93f399696f36d7d6f94bf2a9993a26641fd`（2026-09-02）。
> 本文的模块级复现使用 Issue 附件中的 `atomics_stale_hwerr_repro.zip`。

## 0. 结论

1. `AtomicsUnit` 用寄存器 `exceptionVec` 保存异常位。该寄存器在 `resetFSM()` 中不清零。
2. 原子操作 A 收到 `tl_corrupt=1`、`tl_denied=0` 的 DCache 响应后，`exceptionVec(hardwareError)` 被置 1。
3. 操作 A 写回后执行 `resetFSM()`。`hardwareError` 仍为 1。
4. 操作 B 走早期退出路径（本复现为地址非对齐）。该路径只更新自己的异常位，不更新 `hardwareError`。
5. 操作 B 写回的异常向量同时含 `storeAddrMisaligned=1`（bit 6）和残留的 `hardwareError=1`（bit 19）。
6. ROB 用 `exceptionVec.orR` 判断异常。因此操作 B 携带了前一个操作产生的异常位。
7. 修复在 `resetFSM()` 中清空 `exceptionVec`。修复后，操作 B 的 bit 19 为 0。

## 1. Issue 现象与复现

| 项目 | 内容 |
| --- | --- |
| Issue | [OpenXiangShan/XiangShan#6343](https://github.com/OpenXiangShan/XiangShan/issues/6343) |
| 分支 | `kunminghu-v2`；Issue 说明该问题也存在于 `kunminghu-v3` |
| 报告方式 | 静态分析工具发现；维护者后用模块级复现确认 |
| 复现附件 | `case/atomics_stale_hwerr_repro.zip`，SHA-256 `68aab63dd16f3807f37beadb6cebe578effca730b3c0c87ccee708b953daf59f` |
| 模块 | 生成的 `AtomicsUnit.sv` |
| 复验方式 | 运行附件中的 `obj_orig/VAtomicsUnitOrig`，并运行修复对照 `obj_fixed/VAtomicsUnitFixed` |
| 原始运行日志 | `repro/run.log`，SHA-256 `1e4f981b5e7b7d5871921c1e716d6459f0b68f0c3c7143016cec8ff66b4e9531` |

本次在 Linux x86-64 主机上直接运行附件中的两个 ELF 可执行文件。输出与附件 `run.log` 一致：

```text
=== RUN original ===
cycle 13 opA WB fire bit6=0 bit19=1
cycle 17 opB WB fire bit6=1 bit19=1
PASS[original]: opA bit19=1; opB bit6=1 bit19=1; opB dcache requests=0
=== RUN fixed ===
cycle 13 opA WB fire bit6=0 bit19=1
cycle 17 opB WB fire bit6=1 bit19=0
PASS[fixed]: opA bit19=1; opB bit6=1 bit19=0; opB dcache requests=0
```

`bit6` 是 `storeAddrMisaligned`。`bit19` 是 `hardwareError`。

## 2. 模块级复现的驱动过程

复现 harness 直接驱动 `AtomicsUnit` 的全部外部接口。默认值全为 0，例外为：

- `io_dcache_req_ready=1`
- `io_flush_sbuffer_empty=1`
- `io_csrCtrl_cache_error_enable=1`
- `io_out.ready` 在生成的 RTL 中恒为 1

DTLB 和 PMP 响应为干净命中。

| 周期 | 操作 | 观测 |
| ---: | --- | --- |
| 5 | 操作 A 发 STA | 地址 `0x1000`，`amoswap.d` |
| 6 | TLB 第 1 次响应 | 设计上忽略（硬件预取可能产生） |
| 7 | TLB 第 2 次响应 | 接受 |
| 8 | 操作 A 发 STD | 数据 `0xdeadbeefcafebabe` |
| 10 | 操作 A 发 DCache 请求 | 地址 `0x1000` |
| 11 | DCache 响应 | `miss=0`、`replay=0`、`corrupt=1`、`denied=0` |
| 13 | 操作 A 写回 | bit6=0，bit19=1 |
| 14 | 操作 B 发 STA | 地址 `0x2003`，非 D 对齐 |
| 15 | TLB 第 1 次响应 | 忽略 |
| 16 | TLB 第 2 次响应 | 接受；地址非对齐，提前退出 |
| 17 | 操作 B 写回 | bit6=1，bit19 在原版为 1，在修复版为 0；无 DCache 请求 |

操作 B 没有到达 `s_cache_resp_latch`。因此第 394 行的 `hardwareError := ...` 没有执行。残留值被原样写回。

## 3. RTL 代码证据

以下行号对应基线 `c4cfd2b78`。

`src/main/scala/xiangshan/mem/pipeline/AtomicsUnit.scala`：

```scala
// 100
val exceptionVec = RegInit(0.U.asTypeOf(ExceptionVec()))

// 255-262：TLB 路径只更新这些位，不更新 hardwareError
exceptionVec(loadAddrMisaligned)  := !addrAligned && isLr
exceptionVec(storeAddrMisaligned) := !addrAligned && !isLr
exceptionVec(storePageFault)      := io.dtlb.resp.bits.excp(0).pf.st
exceptionVec(loadPageFault)       := io.dtlb.resp.bits.excp(0).pf.ld
exceptionVec(storeAccessFault)    := io.dtlb.resp.bits.excp(0).af.st
exceptionVec(loadAccessFault)     := io.dtlb.resp.bits.excp(0).af.ld
exceptionVec(storeGuestPageFault) := io.dtlb.resp.bits.excp(0).gpf.st
exceptionVec(loadGuestPageFault)  := io.dtlb.resp.bits.excp(0).gpf.ld

// 392-394：只有到达 s_cache_resp_latch 才会更新 hardwareError
val canTriggerCacheError = dcache_resp_tl_error.asUInt.orR && io.csrCtrl.cache_error_enable
exceptionVec(loadAccessFault)  := canTriggerCacheError && isLr && dcache_resp_tl_error.tl_denied
exceptionVec(storeAccessFault) := canTriggerCacheError && !isLr && dcache_resp_tl_error.tl_denied
exceptionVec(hardwareError)    := canTriggerCacheError &&
  dcache_resp_tl_error.tl_corrupt && !dcache_resp_tl_error.tl_denied

// 424-431：resetFSM 不清 exceptionVec
def resetFSM(): Unit = {
  state := s_invalid
  out_valid := false.B
  data_valid := false.B
  stdCnt := 0.U
  pdest1Valid := false.B
  pdest2Valid := false.B
}

// 485：整个向量写回 ROB
io.out.bits.uop.exceptionVec := exceptionVec
```

`src/main/scala/xiangshan/package.scala`：

```scala
// 832-844
def storeAddrMisaligned = 6
def storeAccessFault    = 7
def storePageFault      = 15
def hardwareError       = 19
```

`src/main/scala/xiangshan/backend/rob/Rob.scala`：

```scala
// 1227
exc_wb.bits.hasException := wb.bits.exceptionVec.get.asUInt.orR
```

ROB 不区分异常来源。任意一位为 1 就表示该指令有异常。

## 4. 修复

PR #6316 的第二个修复点在 `resetFSM()` 中清空异常向量：

```scala
// cbf2d93f3，AtomicsUnit.scala 第 465 行
def resetFSM(): Unit = {
  ...
  exceptionVec := ExceptSparseVec.zeros(param.exceptionOut)
}
```

该修改使每个被接受的原子操作从全零异常向量开始。操作 B 不再携带操作 A 的 `hardwareError`。

修复后的模块级对照由附件中的 `AtomicsUnit_fixed.sv` 和 `obj_fixed/VAtomicsUnitFixed` 给出。其唯一插桩就是上述清零条件。`make_fixed.py` 在原生成的 RTL 中插入该条件；若插入点不匹配，脚本失败。

## 5. 影响范围

维护者指出：`hardwareError` 的优先级低于多数异常。因此，当操作 B 同时有地址非对齐、access fault 等更高优先级异常时，架构上报的 cause 可能仍是那些异常。真正受影响的路径是 `guest load/store page fault`。在这些路径上，残留的 `hardwareError` 可能改变异常分类或产生多余异常。

本复现只证明“异常向量携带残留位”。它不证明每一种原子操作都会改变架构 cause。模块级测试不能替代全核异常优先级验证。

## 6. NEMU、QEMU 与其他处理器

本问题不适用 NEMU/QEMU 对照：

- `tl_corrupt` 是 TileLink 响应错误，属于实现相关的缓存错误。
- NEMU 和 QEMU 不模拟 XiangShan 的 `AtomicsUnit` 状态机，也不产生该残留位。
- 正确性判据是 RISC-V 的精确异常要求：一条指令的异常向量不应包含前一条指令的状态。该要求由模块级定向测试检查。

## 7. 限制

- 这是模块级复现，不是全核复现。它直接驱动 `AtomicsUnit` 的接口，不覆盖 ROB、IQ、DCache 的真实时序。
- 原版 `AtomicsUnit.sv` 不在附件中。附件提供原版和修复版的可执行文件、修复版 SV、harness 和日志。若要从源码重建原版，需要在 `c4cfd2b78` 上生成 `build/rtl/AtomicsUnit.sv`。
- 本次在 Verilator 5.052 主机上运行了预编译的可执行文件。原附件使用 Verilator 5.048。两者的运行结果一致，但生成 RTL 的校验和属于附件记录，不是本次重建。
- 修复验证只覆盖“操作 A corrupt、操作 B 非对齐”这一条轨迹。

## 8. 复现材料

| 文件 | 说明 |
| --- | --- |
| `case/atomics_stale_hwerr_repro.zip` | Issue 原始附件（含原版和修复版可执行文件） |
| `repro/tb_atomics.cpp` | 模块级 harness |
| `repro/make_fixed.py` | 生成修复对照的脚本 |
| `repro/difftest_stub.sv` | 缺失的 difftest 事件 sink 桩 |
| `repro/AtomicsUnit_fixed.sv` | 修复对照 RTL |
| `repro/README.md` | 原始复现说明 |
| `repro/run.log` | 原始运行日志 |
| `repro/run.sh` | 从用户提供的 `AtomicsUnit.sv` 重建并运行 |
