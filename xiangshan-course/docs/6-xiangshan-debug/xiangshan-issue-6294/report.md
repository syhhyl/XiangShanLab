# XiangShan Issue #6294：store page fault 的 AMO 并未改坏 rd；真正原因是跨 16B load 丢失 store-to-load forwarding

> **基线**：昆明湖 V3，`4cf8b8d4ae343e01dfb54e5ae8febdaeaa9992ae`（2026-07-24），xs-env `33d5f6f611d15a65c6194290fa62caf0c0c27f41`，`DefaultConfig`。
> Issue 报告在 `2b5769e8b2`（2026-07-20）和 `d97789de12` 上复现。本次复验使用 `4cf8b8d4ae`。三个提交都在 Issue #6229 未修复的区间。本文的周期号和 handler PC 只属于 `4cf8b8d4ae`。

## 0. 结论

1. `amoxor.w x9,x13,(x9)` 在 `0x2b5c` 取 store page fault（cause `0xf`，`stval=0x20008`）。这一点 DUT 与 NEMU 一致。
2. DiffTest 随后报告 `s1`（x9）不一致：NEMU 为 `0x20008`，DUT 为 `0`。报告位置为 `pc=0xffffffffffe02370`。
3. 故障 AMO 的推测写回不是根因。trap handler 的 store 数据仍是 `0x20008`。这说明架构 x9 没有被改坏；redirect 丢弃了推测写回。
4. 真正原因在 trap handler 的跨 16B misaligned load。store 地址为 `0xffffffffffe650ca`，8 字节访问跨过 `0xffffffffffe650d0` 的 16B 边界。
5. 该 load 被拆成 lower half `0x...650ca` 和 upper half `0x...650d0`。lower half 的 SQ/SBuffer forward mask 为 0。它去读 DCache，得到旧数据 0。
6. upper half 的 forward mask 为 `0xfc03`，forward data 含正确的 `0x20008`。但 lower half 的旧数据没有被修正。最终写回 `0`。
7. 该机制与 Issue #6229 相同。维护者在 #6294 的评论中也给出同一判断。Issue #6294 已关闭；#6229 在 2026-11-09 仍为 open。

## 1. Issue 现象与复现

| 项目 | 内容 |
| --- | --- |
| Issue | [OpenXiangShan/XiangShan#6294](https://github.com/OpenXiangShan/XiangShan/issues/6294) |
| 分支 | `kunminghu-v3` |
| 报告基线 | `2b5769e8b2`、`d97789de12` |
| 本次基线 | `4cf8b8d4ae` |
| 输入 | `case/repro_amo_fault_clobber.elf` |
| 输入 SHA-256 | `07e2d159ca9b47a097d3350a1c03d26f735360c4e3fba26437c76952c567b04a` |
| 原始附件 | `xs_amo_fault_clobbers_rd.zip`，SHA-256 `1e50fa31a9bd775dc408da5c991d6a46db09472f83a0cae9ec96e651acca14fb` |
| 参考模型 | `ready-to-run/riscv64-nemu-interpreter-so`（bundled） |
| 波形 | `replay.fst`，SHA-256 `f1229a330536880a367c171b6a0249890322964b530229c3626384c5c666ef8d` |
| 日志 | `replay.log`，SHA-256 `da060fd70dfa15bab71a18ab9315a35b967a12cae60a56034ee1ebc87315d7b6` |

运行命令：

```bash
emu -i case/repro_amo_fault_clobber.elf \
  --diff ready-to-run/riscv64-nemu-interpreter-so \
  -C 500000 --dump-wave-full --wave-path replay.fst
```

DiffTest 在 `instrCnt=7246`、`cycleCnt=12237` 时终止：

```text
s1 different at pc = 0xffffffffffe02370,
right = 0x0000000000020008, wrong = 0x0000000000000000
```

`right` 是 NEMU 的值。`wrong` 是 DUT 的值。该输出与 Issue 中维护者的记录一致。

## 2. 程序与异常序列

反汇编 `case/repro_amo_fault_clobber.elf` 得到关键序列：

```asm
# 故障 AMO
80002b5c: 20d4a4af    amoxor.w s1,a3,(s1)      # s1 = 0x20008

# trap entry：保存寄存器到 sscratch 指向的栈
8000047c: 14011173    csrrw sp,sscratch,sp
8000048e: e4a6        sd s1,72(sp)
800004c8: 141022f3    csrr t0,sepc
800004cc: e616        sd t0,264(sp)             # sepc = 0x2b5c
800004ce: 143022f3    csrr t0,stval
800004d2: ea16        sd t0,272(sp)             # stval = 0x20008

# handle_trap：保存 s1 到 sp+88
80002394: eca6        sd s1,88(sp)              # 地址 0xffffffffffe650ca

# 之后从同一地址恢复 s1
80002474: 64e6        ld s1,88(sp)              # 地址 0xffffffffffe650ca
80002494: 64e6        ld s1,88(sp)              # 地址 0xffffffffffe650ca
```

`sp` 在 `handle_trap` 中为 `0xffffffffffe65072`。`sp+88 = 0xffffffffffe650ca`。该地址不是 8 字节对齐，且 8 字节访问跨过 16B 边界 `0xffffffffffe650d0`。

提交轨迹显示：

| 序号 | 事件 |
| ---: | --- |
| [04] | `csrr t0,sepc` 得到 `0x2b5c` |
| [06] | `csrr t0,stval` 得到 `0x20008` |
| [12] | `handle_trap` 入口 |
| [20] | `sd s1,88(sp)` |
| [27] | `ld s1,88(sp)` 提交 `data=0` |
| — | DiffTest 在 `pc=0xffffffffffe02370` 报告 `s1` 不一致 |

## 3. 波形证据

所有值在 `TOP.clock` 的上升沿采样。相关 load 在 `inner_LoadUnit_0` 执行。地址信号宽度为 39 位，因此 `0xffffffffffe650ca` 在波形中记为 `0x3ffffffe650ca`。

### 3.1 load 被拆成两个请求

| 周期 | 观测 | 信号 |
| ---: | --- | --- |
| 12279 | S0 请求 `0x3ffffffe650ca`，发 DCache 请求 | `io_sqForward_s0Req_bits_vaddr`、`io_dcache_req_bits_vaddr` |
| 12280 | S0 请求 `0x3ffffffe650d0`，发 DCache 请求 | 同上 |
| 12281 | lower half 的 S2 forward mask：SQ=`0x0000`，SBuffer=`0x0000` | `io_sqForward_s2Resp_bits_forwardMask_*`、`io_sbufferForward_s2Resp_bits_forwardMask_*` |
| 12282 | upper half 的 S2 forward：SQ mask=`0xfc03`，SQ data 含 `0x20008`；SBuffer mask=`0xfc00` | 同上 |
| 12283 | load 写回 `io_ldout_toIntRf_bits_data=0`，pdest=`95`（x9 的物理寄存器） | `io_ldout_toIntRf_*` |

lower half 的 mask 为 0。`NewLoadUnit` 因此判定没有 full forward，访问 DCache。DCache 返回旧数据。`mergeData` 在没有 mask 的字节上保留旧数据，所以 lower half 保持 0。

upper half 在 12282 得到 `0xfc03`，其 forward data 中有正确的 `0x20008`。但此时 head 部分已经按旧数据处理。最终写回仍是 0。

### 3.2 store 数据是 0x20008

12282 的 SQ forward data 为：

```text
0x2000800000000000000000000
```

按字节展开为：

```text
byte 10 = 0x08, byte 11 = 0x00, byte 12 = 0x02
```

这对应 8 字节值 `0x20008` 在地址 `0x...650ca` 的低 6 字节。SQ 中保存的是正确值。因此，trap handler 的 `sd s1,88(sp)` 写入了 `0x20008`。架构 x9 没有被故障 AMO 改坏。

### 3.3 最终不一致

12283 的 `io_ldout_toRob_bits_debugInfo_vaddr` 为 `0x3ffffffe650ca`，`io_ldout_toIntRf_valid=1`，`io_ldout_toIntRf_bits_data=0`。该值写入物理寄存器 95。之后 DiffTest 报告 x9（s1）为 0，而不是 `0x20008`。

## 4. RTL 代码证据

以下行号对应基线 `4cf8b8d4ae`。

`src/main/scala/xiangshan/mem/pipeline/NewLoadUnit.scala`：

```scala
// 956-958
val storeForwardMask = io.sqForwardResp.bits.forwardMask.asUInt |
  io.sbufferForwardResp.bits.forwardMask.asUInt |
  io.uncacheForwardResp.bits.forwardMask.asUInt

// 983-984
val storeFullForward = (~storeForwardMask & in.mask) === 0.U && !sqDataInvalid
val fullForward = storeFullForward || dcacheFullForward
val needDCacheAccess = !fullForward && !isUncache && !isUncacheReplay
```

lower half 的 `storeForwardMask=0`。`in.mask` 覆盖该 half 的字节。`storeFullForward=0`。因此 `needDCacheAccess=1`。load 去读 DCache，而不是等待 store 数据。

```scala
// 1737-1745
val sqForwardMask = io.s2SqForwardResp.bits.forwardMask.asUInt
val sqForwardData = io.s2SqForwardResp.bits.forwardData.asUInt
val sbufferForwardMask = io.s2SbufferForwardResp.bits.forwardMask.asUInt
val sbufferForwardData = io.s2SbufferForwardResp.bits.forwardData.asUInt

// 1753
val s2Data = mergeData(rawData, datas, masks)

// 1789-1797
def mergeData(oldData: UInt, newData: Seq[UInt], mask: Seq[UInt]): UInt = {
  VecInit((0 until bytesNum).map { case i =>
    val sels = mask.map(_(i).asBool) :+ true.B
    val bytes = newData.map(getByte(_, i)) :+ getByte(oldData, i)
    ParallelPriorityMux(sels, bytes)
  }).asUInt
}
```

`mergeData` 在 mask 为 0 的字节上选择 `oldData`。`oldData` 是 DCache 返回的旧数据。代码没有在 mask 不完整时强制 replay。因此错误的 lower half 数据可以提交。

`src/main/scala/xiangshan/mem/pipeline/LoadUnit.scala` 定义 `dcache_miss = cause(C_DM)`。本次 lower half 的路径是“未 forward + DCache 访问”，不是 nuke 或 match invalid。

## 5. 为什么报告中的“AMO 改坏 rd”不成立

报告人的推理是：故障 AMO 仍驱动整数写回，所以 rd 被写坏。该写回可能确实发生，但它发生在 redirect 之前。此时物理寄存器仍属于 AMO 的推测映射。redirect 恢复 RAT 后，架构 x9 回到旧映射。

本次波形给出直接反证：

- trap handler 的 `sd s1,88(sp)` 数据为 `0x20008`。
- 该 store 数据来自架构 x9。
- 如果 AMO 改坏了架构 x9，store 数据应为 0。
- 因此 x9 在 handler 中仍为 `0x20008`。

真正的 0 来自随后同地址的 load。lower half 未 forward，读到了旧 DCache 数据。

## 6. 与其他 Issue 和修复状态

- [#6229](https://github.com/OpenXiangShan/XiangShan/issues/6229) 记录同一机制：跨 16B misaligned load 的 lower half 丢失 store-to-load forwarding，提交旧数据。
- #6294 的维护者评论（2026-08-05）已把根因归到 #6229。Issue 在 2026-08-11 关闭。
- #6229 在 2026-11-09 仍为 open。没有确认的修复提交。
- 提交 `e1424686a`（`fix(mem): suppress atomic register writes on exceptions`）在异常时抑制 AMO 的 `rfWen`。该提交只在 `origin/codex/memblock-ut-closure-20260905` 上，不是 `kunminghu-v3` 的祖先。它处理推测写回，不修复 #6229 的 forwarding 丢失。

## 7. NEMU、QEMU 与其他处理器

| 模型 | 结果 | 说明 |
| --- | --- | --- |
| NEMU（DiffTest REF） | `s1=0x20008` | 正确的架构值；DUT 为 0 |
| QEMU `qemu-system-riscv64` | 启动并执行 boot 代码，30 s 内无自检输出 | 该 ELF 依赖 XiangShan 的 tohost/PMEM 约定；QEMU 不能作为完成判据 |
| Test-infra 多核结果 | 未运行 | 该 ELF 没有 success/fail 自检端点，harness 标记为 `not run` |

QEMU 的 `-d in_asm` 记录显示它执行到 `0x800000e8` 的 `vsetvli`，然后超时。该记录只说明 QEMU 可以开始执行，不说明它复现或修复了本问题。

QEMU 命令：

```bash
qemu-system-riscv64 -M virt -cpu rv64 -m 2G -nographic \
  -bios none -kernel case/repro_amo_fault_clobber.elf
```

## 8. 限制

- 本次只复验一个 ELF。Issue 的原始波形没有公开，无法逐周期对比。
- 本次基线是 `4cf8b8d4ae`，不是 Issue 的 `2b5769e8b2`。两者的失败签名相同（`s1: 0x20008 vs 0`，`pc=0xffffffffffe02370`），但 handler PC 和周期号不同。
- 没有在 #6229 修复后版本上复验。
- 没有独立验证故障 AMO 的推测写回时序。本文用 store 数据证明架构 x9 未被改坏，因此该时序不是结论的依赖。
- 波形中的地址为 39 位。本文同时给出 39 位值和符号扩展后的 64 位值。

## 9. 复现材料

| 文件 | 说明 |
| --- | --- |
| `case/repro_amo_fault_clobber.elf` | Issue 附件中的复现 ELF |
| `case/repro_amo_fault_clobber.si` | 原始指令序列 |
| `case/amo_fault_evidence.txt` | 报告人的说明 |
| `case/nemu_reftrace_excerpt.txt` | 原始 NEMU trace |
| `case/qemu.log`、`case/qemu-asm.log` | QEMU 启动执行记录（超时） |
| `case/attachments.json` | 原始附件 URL 和 SHA-256 |
| `replay.sh` | 固定提交、构建并运行 |
| `replay.log` | 本次运行日志 |
| `waveform-verification/verify.py` | 从 `replay.fst` 复验本文关键检查 |
| `waveform-verification/evidence.json` | 提取结果 |

`replay.fst` 未纳入课程仓库。运行 `./replay.sh` 重新生成。
