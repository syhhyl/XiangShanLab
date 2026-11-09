# 波形复验方法（Issue #6289）

- 解释器：`/nfs/home/sunyuhang/wavekit/.venv/bin/python`，Python 3.12.3
- Wavekit：0.8.3，安装路径 `/nfs/home/sunyuhang/wavekit/src/wavekit`，checkout `60de5e0`
- 波形：`replay.fst`，FST，SHA-256 `100bbda3b4212f7286ae562da973242203f6ebd09d3d39a265cfe83a99f1753d`
- 时钟：`TOP.clock`
- 采样：上升沿（`sample_on_posedge=True`）
- 复位：不额外排除。分析窗口从仿真起点开始，关键事件在 5193–5362 周期
- 窗口：绝对采样周期 `[0, 20353]`，共 20354 个采样点
- 时间单位：FST 原生 tick，0–40706；一个时钟周期 2 个 tick。未把 tick 解释为 ns
- X/Z：对 `state`、`io_sqempty`、`io_flush_empty`、`ctrlEntries_3_hasException` 调用 `load_unknown_mask`，未知值为 0 个

复验命令：

```bash
cd <issue-dir>
~/wavekit/.venv/bin/python waveform-verification/verify.py \
  --wave replay.fst --json waveform-verification/evidence.json
```

脚本检查 13 项，全部 PASS。`evidence.json` 保存检查结果和提取值。

输入文件在运行前后未变化。波形覆盖有限仿真窗口。本次复验证明窗口内的队列停滞和指针状态，不单独证明无限时间性质。
