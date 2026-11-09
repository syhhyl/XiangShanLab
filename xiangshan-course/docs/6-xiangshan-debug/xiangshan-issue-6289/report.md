# XiangShan Issue #6289：异常 vector store 残留物理 Store Queue 队头，导致后续 AMO 永久等待

> **基线**：昆明湖 V3，`4cf8b8d4ae343e01dfb54e5ae8febdaeaa9992ae`（2026-07-24），xs-env `33d5f6f611d15a65c6194290fa62caf0c0c27f41`，`DefaultConfig`。
> 本文结论只对该提交和该输入成立。Issue 报告也使用 `4cf8b8d4ae`。

## 0. 结论

1. 测试程序在 `0x800010d0` 执行 `vse32.v v15,(ra)`，地址为 `0x5c0`。该指令取 store access fault（cause 7）。
2. 该 vector store 在物理 Store Queue 的槽 3 留下 `hasException=1`、`committed=0`、`addrValid=1`、`dataValid=1`。
3. 随后的一次 redirect 使用恢复指针 5，没有取消槽 3。
4. 物理队列 `deqPtr` 停在 3。队列不空，`sbuffer.io_sqempty` 一直为 0。
5. 下一条 AMO 在 `0x800010dc` 执行。AtomicsUnit 停在 `s_wait_flush_sbuffer_resp`，等待 sbuffer 变空。它永远等不到。
6. DUT 在 15000 个采样周期内没有提交。DiffTest 让 REF 前进一条指令，报告 `a2` 不一致。

根因与 Issue #6267 同类：VirtualStoreQueue 的 `vecMbCommit` 提前退休了带异常的 vector store。redirect 又保留了它的物理槽。该槽既不能提交，也不能出队。后续 AMO 的 sbuffer flush 握手因此不能完成。

## 1. Issue 现象与复现

| 项目 | 内容 |
| --- | --- |
| Issue | [OpenXiangShan/XiangShan#6289](https://github.com/OpenXiangShan/XiangShan/issues/6289) |
| 分支 | `kunminghu-v3` |
| 报告基线 | `4cf8b8d4ae` |
| 输入 | `case/seeds_48_.elf` |
| 输入 SHA-256 | `3808618dc52fcb507293e94172b92315b390958534d7882cc75f675dada10cf7` |
| 参考模型 | `ready-to-run/riscv64-nemu-interpreter-so`（bundled） |
| 仿真 | `emu --dump-wave-full`，FST 波形 `replay.fst` |
| 波形 SHA-256 | `100bbda3b4212f7286ae562da973242203f6ebd09d3d39a265cfe83a99f1753d` |
| 日志 | `replay.log`，SHA-256 `e9fc479ab25bd8d813a53232d1c8bfa3189746995577f26162299f868c98cf65` |

运行命令：

```bash
emu -i case/seeds_48_.elf \
  --diff ready-to-run/riscv64-nemu-interpreter-so \
  -C 300000 --dump-wave-full --wave-path replay.fst
```

本次运行在 20354 个上升沿采样点后结束。DUT 的 `instrCnt=334`，`cycleCnt=20299`。DiffTest 报告：

```text
a2 different at pc = 0xfffed825b34beec9,
right = 0x00000000c0c0c0c0, wrong = 0xc6fe9d4ace7be0fa
```

`right` 是 NEMU 的值。`wrong` 是 DUT 的值。两者在 `a2` 上不一致。

## 2. 程序触发序列

反汇编 `case/seeds_48_.elf` 得到关键序列：

```asm
8000108c: 192fa12f    sc.w sp,s2,(t6)
80001090: 032108bb    mulw a7,sp,s2
80001094: fd289df3    .word 0xfd289df3        # 非法指令
80001098: 3e3fac27    fsw ft3,1016(t6)
8000109c: 4008dd93    srai s11,a7,0
800010a0: 0b1fad2f    amoswap.w.rl s10,a7,(t6)
...
800010d0: 0200e7a7    vse32.v v15,(ra)        # ra = 0x5c0
800010d4: 02eeecbb    remw s9,t4,a4
800010d8: 014f4d23    .word 0x014f4d23        # 非法指令
800010dc: 0d6fb62f    amoswap.d.aq a2,s6,(t6)
800010e0: c4cfa9af    amominu.w.aq s3,a2,(t6)
```

提交轨迹显示：

| 序号 | 事件 | 说明 |
| ---: | --- | --- |
| [20] | `exception pc 0x800010d0 cause 7` | `vse32.v` 取 store access fault |
| [21]–[25] | 执行 M-mode trap handler | 把 `mepc` 加 4，然后 `mret` |
| [26] | commit `0x800010d4` | 返回后执行 `remw` |
| [27]–[31] | `exception 0x800010d8 cause 2`，再 `mret` | 第二条非法指令，handler 跳过 4 字节 |
| — | 无 | 下一条 `amoswap.d.aq`（`0x800010dc`）不提交 |

DUT 最后提交的是 `0x800010d8` 的异常和它的 handler（`0x80001000`–`0x8000100c`）。之后没有有效提交。`0x800010dc` 的 AMO 是 ROB 头。

## 3. 波形证据

所有值在 `TOP.clock` 的上升沿采样。时间窗口为采样周期 `[0, 20353]`。

### 3.1 物理 Store Queue 槽 3 的状态

| 周期 | 观测 | 信号 |
| ---: | --- | --- |
| 5197 | 槽 3 `dataValid=1` | `physicalQueue.ctrlEntries_3_dataValid` |
| 5199 | 槽 3 `addrValid=1`，`vaddr=0x5c0`，`robIdx=0x12e`，`numLsElem=2`，`size=4`，`byteMask=0xffff` | `ctrlEntries_3_*`、`dataEntries_3_*` |
| 5200 | 槽 3 `hasException` 置 1 | `ctrlEntries_3_hasException` |
| 5204 | `ctrlEntries_3_vecMbCommit` 拉高 | `virtualStoreQueue.ctrlEntries_3_vecMbCommit` |
| 5206 | `retireValidReg=1`，`retireReqNumReg=2` | `virtualStoreQueue.retireValidReg`、`retireReqNumReg` |
| 5207 | `toPhysicalQueueRetirePtr` 从 3 变为 5 | `virtualStoreQueue.toPhysicalQueueRetirePtr_value` |
| 5213 | redirect 有效，ROB 编号为 `0x12e` | `virtualStoreQueue.io_redirect_bits_robIdx_value=302` |
| 5216 | redirect 恢复指针为 5 | `virtualQueue.io_toPhysicalQueue_redirectPtr_bits_value` |
| 20353 | 槽 3 仍为 `hasException=1`、`committed=0` | `ctrlEntries_3_hasException/committed` |

`robIdx=0x12e` 对应提交轨迹中的 `vse32.v`。`numLsElem=2`、`size=4`、`byteMask=0xffff` 表示 16 字节 vector store。`vaddr=0x5c0` 与 `ra` 一致。

### 3.2 队列指针和空状态

| 周期 | 观测 | 信号 |
| ---: | --- | --- |
| 5196 | `deqPtr=3` | `physicalQueue.deqPtrExt_0_value` |
| 5193 | `commitPtr=3` | `physicalQueue.commitPtrExt_0_value` |
| 5184 | `enqPtr=5` | `physicalQueue.io_fromVirtualStoreQueue_physicalQueueEnqPtr_value` |
| 5207 → 20353 | `sqempty=0` | `sbuffer.io_sqempty` |
| 5183 → 20353 | `flush_empty=0` | `sbuffer.io_flush_empty` |
| 5419 → 20353 | `sbempty=1`，但 `sqempty=0` | `sbuffer.io_sbempty`、`io_sqempty` |

`deqPtr`、`rdataPtr`、`commitPtr` 都停在 3。槽 3 是队列头。队列的其余部分没有可出队的项。

### 3.3 AMO 状态

| 周期 | 状态 | 编号 |
| ---: | --- | ---: |
| 5359 | `s_tlb_and_flush_sbuffer_req` | 1 |
| 5361 | `s_pm` | 2 |
| 5362 | `s_wait_flush_sbuffer_resp` | 3 |
| 5362 → 20353 | 保持 `s_wait_flush_sbuffer_resp` | 3 |

`inner_atomicsUnit.state` 从 5362 到仿真结束一直是 3。`io_flush_sbuffer_valid` 一直为 1。`io_flush_sbuffer_empty` 一直为 0。因此状态机没有条件进入 `s_cache_req`。

## 4. RTL 代码证据

以下行号对应基线 `4cf8b8d4ae`。

### 4.1 AtomicsUnit 等待 sbuffer 空

`src/main/scala/xiangshan/mem/pipeline/AtomicsUnit.scala`：

```scala
// 309
state := Mux(sbuffer_empty, s_cache_req, s_wait_flush_sbuffer_resp)

// 318-320
when (state === s_wait_flush_sbuffer_resp) {
  when (sbuffer_empty) {
    state := s_cache_req
  }
}
```

`sbuffer_empty` 来自 `io.flush_sbuffer.empty`。`io.flush_sbuffer.valid` 在 `s_tlb_and_flush_sbuffer_req`、`s_pm`、`s_wait_flush_sbuffer_resp` 三态都为 1（第 501–504 行）。

### 4.2 flush empty 依赖 sqempty

`src/main/scala/xiangshan/mem/MemBlock.scala`：

```scala
// 1356
val stIsEmpty = sbuffer.io.flush.empty && uncache.io.flush.empty

// 1400
atomicsUnit.io.flush_sbuffer.empty := stIsEmpty
```

`src/main/scala/xiangshan/mem/sbuffer/Sbuffer.scala`：

```scala
// 541
val all_empty = cmo_empty && io.sqempty

// 555
io.flush.empty := GatedValidRegNext(all_empty)
```

因此，只要物理 Store Queue 不空，AtomicsUnit 就不能前进。

### 4.3 物理队列不能出队槽 3

`src/main/scala/xiangshan/mem/lsqueue/NewStoreQueue.scala`：

```scala
// 1261
deqPtrVectorInactiveValid(i) := ctrl.committed && ctrl.vecInactive

// 1272
val deqCount = Cat(deqPtrMoveFromSbuffer.asUInt,
                   deqPtrVectorInactiveMove, uncacheMove)
```

`deqPtr` 只在三种情况移动：写入 sbuffer、vector inactive、uncache 完成。三种情况都要求 `committed` 或有效的写请求。槽 3 的 `hasException=1`，提交路径排除它：

```scala
// 1924
!ctrlEntries(ptr).hasException && !ctrlEntries(ptr).waitStoreS2 && ctrlEntries(ptr).allValid

// 1934
ctrlEntries(ptr).committed := commitVec(i) || ctrlEntries(ptr).committed
```

因此 `committed` 永远为 0。槽 3 没有出队路径。

### 4.4 redirect 没有取消槽 3

`src/main/scala/xiangshan/mem/lsqueue/VirtualStoreQueue.scala`：

```scala
// 213
vecCommit(i) := vecCommittmp(i).reduce(_ || _) && !needCancel(i) &&
                !deqCancel && ctrlEntries(i).allocated

// 373
toPhysicalQueueRetirePtrNext := toPhysicalQueueRetirePtr +
  Mux(retireValidReg, retireReqNumReg, 0.U)

// 394-396
when (ctrlEntries(redirectLastValidPtr.value).allocated) {
  redirectRecoverPtr := getEntryEndSqIdx(redirectLastValidPtr)
}.otherwise {
  redirectRecoverPtr := toPhysicalQueueRetirePtrNext
}
```

波形显示 `vecMbCommit` 在 5204 拉高。virtual queue 在 5206 用 `retireReqNumReg=2` 把物理 retire 指针推到 5。5213 的 redirect 属于同一个 vector store（ROB `0x12e`）。恢复指针使用 5，因此保留物理槽 `[3,5)`。槽 3 的异常状态没有被清除。

## 5. 根因判断

已确认的因果链：

1. `vse32.v` 取 store access fault。STA 把异常写入物理槽 3（`hasException=1`）。
2. VMergeBuffer 的 `vecMbCommit` 让 VirtualStoreQueue 提前退休该 vector store。物理 retire 指针 3→5。
3. ROB 收到异常并 redirect。恢复指针取 5，没有取消槽 3。
4. 槽 3 不满足 commit 条件，也不满足 deq 条件。`deqPtr` 停在 3。
5. `sqempty=0`，所以 `flush_sbuffer.empty=0`。
6. 下一条 AMO 进入 `s_wait_flush_sbuffer_resp` 后永远等待。

这是功能性死锁，不是 AMO 自身的异常处理错误。AMO 的 TLB、PMP、DCache 路径都没有启动。

## 6. 与其他 Issue 和修复状态

- 该机制与 [#6267](https://github.com/OpenXiangShan/XiangShan/issues/6267) 相同：异常 vector store 提前退休，物理槽被 redirect 保留。
- #6289 在 2026-11-09 仍为 open。维护者说明 V3 vector 访存仍在开发，暂不处理（Issue 评论 2026-07-28、2026-09-04）。
- 后续提交 #6337（`fix stuck on misalign vector store`）、#6338（`propagate vector store replay metadata`）修改 vector store 路径，但本次没有在修复后版本上复验。不能据此声称 #6289 已修复。
- PR #6316（`cbf2d93f3`）修复 AtomicsUnit 的异常位残留和 access fault 优先级。该修复不清除物理 Store Queue 的异常槽，因此不覆盖本问题。

## 7. NEMU、QEMU 与其他处理器

| 模型 | 结果 | 说明 |
| --- | --- | --- |
| NEMU（DiffTest REF） | 执行 `0x800010dc` 的 AMO，`a2=0xc0c0c0c0` | DUT 的 `a2=0xc6fe9d4ace7be0fa`；DUT 未提交 |
| QEMU `qemu-system-riscv64` | 启动并执行到 boot 代码，30 s 内无自检输出 | 该 ELF 依赖 XiangShan 的 tohost/PMEM 约定，QEMU 不能作为完成判据 |
| Test-infra 多核结果 | CVA6、VexiiRiscv 为 inconclusive；Rocket、BOOM、NutShell、OpenC910、Toooba 为 timeout | 输入没有自检端点，不能给出 pass/fail 结论 |

QEMU 的记录只说明它可以开始执行该 ELF。它不证明其他处理器上也存在同一死锁。

QEMU 命令：

```bash
qemu-system-riscv64 -M virt -cpu rv64 -m 2G -nographic \
  -bios none -kernel case/seeds_48_.elf
```

## 8. 限制

- 本次只复现一个 seed，不能证明所有 vector store 异常都会留下物理槽。
- 波形覆盖 20354 个采样周期。15000 周期无提交是 DiffTest 的检测阈值。源码门控说明队列在有限窗口内没有出队路径，但本文不证明无限时间性质。
- `vecMbCommit` 的上游 VMergeBuffer 时序没有逐信号展开。本文只确认了它的脉冲、retire 指针变化和 redirect 恢复指针。
- 没有在修复后的 V3 提交上运行同一 seed。
- 原始 Issue 的 seed 与本地加载的地址一致，但 DiffTest 的 abort PC 不同。两者都报告相同的 `a2` 值，因此属于同一失败签名。

## 9. 复现材料

| 文件 | 说明 |
| --- | --- |
| `case/seeds_48_.elf` | Issue 附件中的唯一 seed |
| `case/attachments.json` | 原始附件 URL 和 SHA-256 |
| `case/qemu.log` | QEMU `-kernel` 运行记录（超时） |
| `replay.sh` | 固定提交、构建并运行 |
| `replay.log` | 本次运行日志 |
| `waveform-verification/verify.py` | 从 `replay.fst` 复验本文关键检查 |
| `waveform-verification/evidence.json` | 提取结果 |

`replay.fst` 未纳入课程仓库。运行 `./replay.sh` 重新生成。

## 10. 实锤补充（2026-11-09）

在 `~/Test-infra/adapters/bug-evidence/` 新增全核证据包：

```text
xiangshan/xiangshan-vec-store-fault-amo-hang
```

该包在 `4cf8b8d4ae` 上重建了 `TestkitV3Config` FST 模型，并重新运行同一 seed。结果：

- DUT `instrCnt=334`、`cycleCnt=17072`；15000 周期无提交。
- DiffTest 报告 `a2`：NEMU `0xc0c0c0c0`，DUT `0xc6fe9d4ace7be0fa`。
- Testkit 波形锚点 cycle 15000：`atom_state=3`、`sqempty=0`、`flush_empty=0`、`slot3_hasException=1`、`slot3_committed=0`、`deq_ptr=3`、`retire_ptr=5`、`redirect_ptr=5`。

本文的 20354/20299 周期和 5199–5216 指针转移来自 DefaultConfig 波形。Testkit 包从 15000 开始，捕获的是转移之后的稳定死锁状态。两份证据指向同一失败签名。
