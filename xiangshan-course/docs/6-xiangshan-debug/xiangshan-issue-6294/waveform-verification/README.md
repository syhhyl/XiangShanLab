# 波形复验方法（Issue #6294）

- 解释器：`/nfs/home/sunyuhang/wavekit/.venv/bin/python`，Python 3.12.3
- Wavekit：0.8.3，安装路径 `/nfs/home/sunyuhang/wavekit/src/wavekit`，checkout `60de5e0`
- 波形：`replay.fst`，FST，SHA-256 `f1229a330536880a367c171b6a0249890322964b530229c3626384c5c666ef8d`
- 时钟：`TOP.clock`
- 采样：上升沿（`sample_on_posedge=True`）
- 分析单元：`inner_LoadUnit_0`
- 窗口：绝对采样周期 `[0, 12291]`，共 12292 个采样点
- 时间单位：FST 原生 tick；未把 tick 解释为 ns
- X/Z：对 `forwardMask_0`、`forwardMask_10`、`ldout_toIntRf_bits_data`、`ldout_toIntRf_valid` 调用 `load_unknown_mask`，未知值为 0 个

复验命令：

```bash
cd <issue-dir>
~/wavekit/.venv/bin/python waveform-verification/verify.py \
  --wave replay.fst --json waveform-verification/evidence.json
```

脚本检查 6 项，全部 PASS。`evidence.json` 保存检查结果和提取值。

## 地址宽度说明

波形中的 `vaddr` 为 50 位规范形式。`0xffffffffffe650ca` 在波形中记为 `0x3ffffffe650ca`。脚本使用 50 位值匹配。

## 采样边界

lower half 的 S0 请求在 12279 周期。S2 forward 响应在 12281 周期。upper half 的 S0 请求在 12280 周期，S2 响应在 12282 周期。数据写回在 12283 周期。脚本用“最后一次 S0 请求 + 2/3 周期”定位响应，避免把之前 trap 的同类 load 混入。
