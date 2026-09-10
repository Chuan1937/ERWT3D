# Figure Captions / 图注

## Fig1 — ERWT3D Method Framework / ERWT3D方法框架

**Caption**: Overview of the ERWT3D adaptive 3-D seismic data storage framework.
(a) The data probe samples the input volume and estimates LZ4 and RZFP compression performance, selecting the optimal format automatically.
(b) The single-file package stores data in superblock order with embedded axis-plane sections for efficient X/Y/Z access.
(c) The unified reader provides balanced multi-axis slice access from a single file.

**说明**: ERWT3D自适应三维地震数据存储框架概览。(a)数据探针采样输入体并估计LZ4和RZFP压缩性能，自动选择最优格式。(b)单文件包以超块顺序存储数据，嵌入轴向平面切片以实现高效X/Y/Z访问。(c)统一读取器从单个文件提供均衡的多轴切片访问。

---

## Fig2 — Adaptive Representation and Storage / 自适应表示与存储

**Caption**: (a) Physical storage ratio for three datasets. F3 amplitude uses LZ4 (lossless), F3 similarity uses RZFP (error-bounded lossy), and 20GB uses LZ4. Dashed line = raw baseline (ratio = 1).
(b) Planner prediction vs measured physical ratio for 20GB. The planner correctly selects LZ4+XYZ, which achieves a measured ratio of 1.012×. Error bars show the planner's upper-bound estimate.

**说明**: (a)三个数据集的物理存储比率。F3振幅使用LZ4(无损)，F3相似度使用RZFP(误差有界有损)，20GB使用LZ4。虚线=原始基线(比率=1)。(b)20GB的规划器预测与实测物理比率对比。规划器正确选择LZ4+XYZ，实测比率为1.012×。误差线为规划器上界估计。

---

## Fig3 — 20GB Multi-Axis Access / 20GB多轴访问

**Caption**: 20GB multi-axis access performance on SSD (cold Linux/WSL guest page-cache, n=5 runs, mean ± SD).
(a) Random access: Raw storage naturally favors X (contiguous); ERWT3D improves Y access by 2.4×. Raw Z is excluded from quantitative comparison due to extreme cache sensitivity (CV > 90%).
(b) Continuous access: similar pattern with smaller absolute differences.
(c) ERWT3D axis balance: random and continuous access times across X/Y/Z axes show comparable order-of-magnitude performance, demonstrating balanced multi-axis access.

**说明**: 20GB在SSD上的多轴访问性能(冷Linux/WSL客户页缓存，n=5次运行，均值±标准差)。(a)随机访问：原始存储天然有利于X轴(连续)；ERWT3D将Y轴访问提升2.4倍。原始Z轴因极端缓存敏感性(CV>90%)被排除在定量比较之外。(b)连续访问：类似模式但绝对差异更小。(c)ERWT3D轴平衡：X/Y/Z轴的随机和连续访问时间在同一数量级，证明了均衡的多轴访问。

---

## Fig4 — Codec Ablation / 编解码器消融

**Caption**: Codec ablation study on 20GB (SSD, n=5 runs).
(a) Physical storage ratio: Auto LZ4 and Forced LZ4 achieve identical 1.012×; Forced RZFP achieves 1.768×.
(b) Composite access time (mean of 6 workloads): individual run points shown. LZ4 provides faster access than RZFP.
(c) XYZ random access comparison: LZ4 vs RZFP across X/Y/Z axes. The planner's selection of LZ4 simultaneously provides lower storage overhead, lossless reconstruction, and faster multi-axis access.

**说明**: 20GB编解码器消融研究(SSD，n=5次运行)。(a)物理存储比率：Auto LZ4和Forced LZ4均为1.012×；Forced RZFP为1.768×。(b)复合访问时间(6个工作负载的均值)：显示单次运行点。LZ4提供比RZFP更快的访问。(c)XYZ随机访问比较：LZ4与RZFP在X/Y/Z轴上的对比。规划器选择LZ4同时提供了更低的存储开销、无损重建和更快的多轴访问。

---

## Fig5 — F3 Real Seismic Data / F3真实地震数据

**Caption**: F3 dataset visualization and per-slice access latency.
(a) F3 amplitude representative Z-slice (blue-white-red diverging colormap, ±99th percentile).
(b) F3 similarity representative Z-slice (cividis sequential colormap).
(c) Per-slice latency for both datasets across X/Y/Z axes, random and continuous patterns (n=5 runs per group, box + scatter). F3 amplitude uses LZ4+XYZ (lossless); F3 similarity uses RZFP+XYZ (lossy, max_rel < 0.001).
(d) Summary statistics table.

**说明**: F3数据集可视化与逐切片访问延迟。(a)F3振幅代表性Z切片(蓝-白-红发散色图，±99百分位)。(b)F3相似度代表性Z切片(cividis顺序色图)。(c)两个数据集在X/Y/Z轴上的逐切片延迟，随机和连续模式(每组n=5次运行，箱线图+散点)。F3振幅使用LZ4+XYZ(无损)；F3相似度使用RZFP+XYZ(有损，max_rel<0.001)。(d)汇总统计表。

---

## Fig6 — RZFP Error-Bound Verification / RZFP误差界验证

**Caption**: RZFP error-bound verification for 20GB and F3 similarity datasets.
(a) Maximum relative error: both datasets satisfy the contest bound of 1×10⁻³ (dashed line). 20GB: 1.000×10⁻³; F3 similarity: 9.989×10⁻⁴.
(b) NRMSE: 20GB: 9.12×10⁻⁵; F3 similarity: 1.91×10⁻⁴. Violations = 0 for both datasets.

**说明**: 20GB和F3相似度数据集的RZFP误差界验证。(a)最大相对误差：两个数据集均满足竞赛界1×10⁻³(虚线)。20GB：1.000×10⁻³；F3相似度：9.989×10⁻⁴。(b)NRMSE：20GB：9.12×10⁻⁵；F3相似度：1.91×10⁻⁴。两个数据集的违规数均为0。
