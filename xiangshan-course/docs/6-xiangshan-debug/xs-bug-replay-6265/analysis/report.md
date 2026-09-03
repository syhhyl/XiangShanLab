# XiangShan Issue #6265 分析报告

## 一句话结论

这不是 XiangShan 算错了地址，而是 XiangShan 和 NEMU 对同一次非法访问选择了不同的异常类型。

目标地址 `0xf1` 同时存在两个问题：它不满足 8 字节访问的对齐要求，也不属于允许访问的 PMA 区域。NEMU 优先报告地址未对齐，XiangShan 则先检查地址是否允许访问，并报告访问失败。RISC-V 规范允许实现自行选择这两种异常的相对优先级，因此该 issue 最终被确认为预期行为，不需要修复 RTL。

## 1. 问题现象

触发异常的指令位于 `0x800012f8`：

```text
机器码：0x0f0fbc03
指令：  ld s8, 240(t6)
```

执行时 `t6=0x1`，所以有效地址为：

```text
0x1 + 0xf0 = 0xf1
```

`ld` 需要读取 8 字节，地址通常应为 8 的倍数，但：

```text
0xf1 & 0x7 = 1
```

因此地址没有对齐。与此同时，系统普通内存从 `0x80000000` 一带开始，`0xf1` 也不在允许读取的 PMA 区域中。

简单说，这个地址既“没有对齐”，又“没有访问资格”。

## 2. XiangShan 与 NEMU 的差异

| 项目 | XiangShan | NEMU |
|---|---:|---:|
| 出错指令 PC | `0x800012f8` | `0x800012f8` |
| 指令 | `ld s8,240(t6)` | `ld s8,240(t6)` |
| `t6` | `0x1` | `0x1` |
| 有效地址 | `0xf1` | `0xf1` |
| `mcause` | `5`，Load access fault | `4`，Load address misaligned |
| `mtval` | `0xf1` | `0xf1` |

两个异常的含义是：

- `mcause=4`：8 字节访问使用了未对齐地址。
- `mcause=5`：这个物理地址不允许读取。

两边对指令和地址的判断完全一致，差别只在于两个问题同时存在时先报告哪一个。

日志最终在 `0x80001314` 报出 difftest 不一致：

```text
mcause different at pc = 0x0080001314,
right = 0x0000000000000004,
wrong = 0x0000000000000005
```

这里的 `0x80001314` 是 difftest 比较出状态差异时程序已经运行到的位置，不是最初触发异常的指令。真正的异常现场仍是：

```text
mepc  = 0x800012f8
mtval = 0xf1
```

## 3. wavekit 波形验证

本次使用 `../../xs/wavekit` 的 `FstReader` 读取 replay 生成的波形：

```text
波形：results/seed/seed.fst
时钟：TOP.SimTop.clock
时间范围：0 - 14241
```

### 3.1 Load Unit 内部结果

目标访问由 LoadUnit 0 执行，对应 ROB index 为 `0x8c`。wavekit 提取到的关键流水线状态如下：

| 位置 | 周期 | ROB | 地址 | 结果 |
|---|---:|---:|---:|---|
| S0 输出 | 7105 | `0x8c` | `0xf1` | `align=0` |
| S1 输出 | 7106 | `0x8c` | `0xf1` | 地址和异常信息继续传递 |
| S2 输出 | 7107 | `0x8c` | `0xf1` | `exceptionVec[4]=0`，`exceptionVec[5]=1` |

其中：

- `exceptionVec[4]` 对应 Load address misaligned。
- `exceptionVec[5]` 对应 Load access fault。

波形证明，Load Unit 送往后端的结果已经是：

```text
loadAddrMisaligned = 0
loadAccessFault    = 1
```

因此并不是异常 4 和异常 5 同时送到 CSR 后，CSR 再选择了异常 5。真正的选择在 Load Unit 内已经完成。

### 3.2 最终 trap 结果

wavekit 对 DUT trap 信号的采样结果为：

| 周期 | 仿真时间 | `trap_cause` | `trap_tval` | 当前 PC |
|---:|---:|---:|---:|---:|
| 7117 | 14234 | `0x5` | `0xf1` | `0x80001314` |
| 7118 | 14236 | `0x5` | `0xf1` | `0x80001314` |
| 7119 | 14238 | `0x5` | `0xf1` | `0x80001314` |
| 7120 | 14240 | `0x5` | `0xf1` | `0x80001314` |

emulator 的异常 trace 同样记录了真正的出错指令：

```text
exception pc 0x800012f8 inst 0x0f0fbc03 cause 0x5
```

波形和运行日志相互印证：XiangShan 对地址 `0xf1` 产生了 Load access fault。

## 4. XiangShan 为什么选择 access fault

### 4.1 S0 先检查对齐

Load Unit 在 S0 根据访问大小检查地址低位：

```scala
val align = LookupTree(size, List(
  "b00".U -> true.B,
  "b01".U -> (bankOffset.take(1) === 0.U),
  "b10".U -> (bankOffset.take(2) === 0.U),
  "b11".U -> (bankOffset.take(3) === 0.U)
))
```

代码位置：[NewLoadUnit.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/mem/pipeline/NewLoadUnit.scala:310)。

`ld` 是 8 字节访问，需要地址低 3 位为 0。`0xf1` 的低 3 位是 `001`，所以 S0 得到 `align=0`。这与 wavekit 在周期 7105 观察到的结果一致。

此时硬件只是记录“地址未对齐”，还没有决定最终上报哪一种异常。

### 4.2 PMA/PMP 检查访问资格

PMA 描述不同物理地址区域是否可读、可写、可执行或可缓存。普通内存范围从 `0x80000000` 开始，`0xf1` 不属于可读内存区域。

PMA 对 load 的检查逻辑为：

```scala
resp.ld := TlbCmd.isRead(cmd) && !cfg.r
```

代码位置：[PMA.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/backend/fu/PMA.scala:210)。

意思是：如果当前操作是读，但地址区域没有读权限，就产生 load access fault 条件。

PMPChecker 将 PMP、PMA 和 KeyID 的拒绝结果合并：

```scala
val resp = if (pmpUsed) (
  resp_pmp | resp_pma | resp_keyid
) else (
  resp_pma | resp_keyid
)
```

代码位置：[PMP.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/backend/fu/PMP.scala:616)。

MemBlock 将这些检查结果连接到 DTLB 和 Load Unit，见 [MemBlock.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/mem/MemBlock.scala:707) 和 [MemBlock.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/mem/MemBlock.scala:921)。

### 4.3 Load Unit 生成最终异常位

关键选择发生在 Load Unit S2：

```scala
val tlbUnaccessable =
  uop.exceptionVec(loadAccessFault) ||
  uop.exceptionVec(loadPageFault) ||
  uop.exceptionVec(loadGuestPageFault)
val tlbAccessable = !tlbUnaccessable

val pmpUnaccessable = pmp.ld && tlbHit
val isNC = tlbHit && tlbAccessable && Pbmt.isNC(pbmt)

val afUnaccessable =
  uop.exceptionVec(loadAccessFault) || pmpUnaccessable
val af = afUnaccessable || afVectorUncache ||
  afUnalignMMIO || afTagError || afForwardDenied || afBypassDenied

val am =
  !in.align.get &&
  accessType.isScalar() &&
  isNC &&
  !pmpUnaccessable

exceptionVec(loadAddrMisaligned) := am
exceptionVec(loadAccessFault) := af
```

代码位置：[NewLoadUnit.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/mem/pipeline/NewLoadUnit.scala:920)。

这段逻辑可以直接理解为：

```text
只有地址属于允许处理的非缓存区域，
并且 PMA/PMP 没有拒绝访问时，
未对齐的标量 load 才报告 misaligned。

如果 TLB 已经带回 access fault，
或者当前 PMA/PMP 检查拒绝访问，
就报告 access fault，并阻止 misaligned 异常生成。
```

原因体现在 `am` 的两个条件中：

- 前级 access fault 会令 `tlbAccessable=0`，进而令 `isNC=0`。
- 当前 PMA/PMP 拒绝会令 `pmpUnaccessable=1`，不满足 `!pmpUnaccessable`。

两条路径都会使 `am=0`。对于本例：

```text
地址未对齐                 = 是
标量 load                  = 是
地址未被 access fault 拒绝 = 否
地址可作为 NC 访问         = 否

最终：
am = 0
af = 1
```

这就是 XiangShan “access fault 优先”的具体实现。

### 4.4 CSR 只负责最终编码

异常编号定义为：

```scala
def loadAddrMisaligned = 4
def loadAccessFault    = 5
```

全局异常优先表中，`loadAddrMisaligned` 实际上排在 `loadAccessFault` 前面：

```scala
def priorities = Seq(
  ...
  loadAddrMisaligned,
  ...
  loadAccessFault,
  hardwareError
)
```

代码位置：[package.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/package.scala:1037) 和 [package.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/package.scala:1102)。

CSR 按这张表从异常向量中编码最终异常号：

```scala
val regularExceptionNO = ExceptionNO.priorities.foldRight(0.U)(
  (i: Int, sum: UInt) => Mux(hasExceptionVec(i), i.U, sum)
)
```

代码位置：[CSR.scala](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/workspace/xiangshan/src/main/scala/xiangshan/backend/fu/CSR.scala:1331)。

如果异常 4 和异常 5 真的同时送到 CSR，异常 4 会优先。本例最终得到异常 5，是因为 Load Unit 送出的异常向量已经是 bit 4 为 0、bit 5 为 1。

完整路径如下：

```text
ld s8,240(t6)
    |
    +-- 计算有效地址 0xf1
    |
    +-- S0 对齐检查：align=0
    |
    +-- PMA/PMP 检查：地址不可读
    |
    +-- Load Unit：am=0，af=1
    |
    +-- ROB 保存 exceptionVec[5]
    |
    +-- CSR 编码为 mcause=5，mtval=0xf1
```

## 5. 为什么这样设计

RISC-V 规范允许实现自行决定 misaligned exception 和 access fault 的相对优先级，主要对应两种设计思路：

- 完全不支持未对齐访问的实现，可以先做对齐检查，直接报告 `mcause=4`。
- 只允许某些物理区域处理未对齐访问的实现，需要先检查地址属性和权限；地址本身不可访问时，可以先报告 `mcause=5`。

NEMU 采用第一种处理方式，XiangShan 采用第二种。

维护者在 issue 中说明，非 PMA 区域本来就不允许访问。如果未授权软件故意向这类区域发起未对齐访问，XiangShan 希望首先将其归类为非法地址访问，并在保护检查处终止，避免地址继续进入后续未对齐拆分和访存流程，降低触发严重硬件错误的风险。

换句话说，XiangShan 的判断顺序是：

```text
1. 这个地址有没有资格被访问？
2. 如果可以访问，它是否对齐，是否需要拆分？
```

这与 Load Unit 中 `isNC` 和 `!pmpUnaccessable` 的条件一致。

## 6. 复现结果与最终判断

在 XiangShan commit `b90dbba40d`、`DefaultConfig` 和配套 NEMU reference 下运行 seed，得到：

```text
mcause different at pc = 0x0080001314,
right = 0x0000000000000004,
wrong = 0x0000000000000005

instrCnt = 518
cycleCnt = 7066
```

结果与 issue 附带的 `seeds.log` 一致。完整运行日志见 [run.log](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/results/seed/run.log:189)，波形见 [seed.fst](/nfs/home/sunyuhang/bug-replay/xs-bug-replay-6265/results/seed/seed.fst)。

最终判断是：

> 同一次访问同时满足“地址未对齐”和“地址不可访问”两个条件。NEMU 直接报告未对齐；XiangShan 先检查 PMA/PMP，如果地址不可访问，Load Unit 就抑制 misaligned 异常，只生成 access fault。这是 RISC-V 允许的实现选择，不是 XiangShan RTL 功能错误。
