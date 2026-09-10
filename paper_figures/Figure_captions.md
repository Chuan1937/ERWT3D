# Figure Captions / 图注

## Fig.1 — ERWT3D Method Framework / ERWT3D方法框架

**English**: Overview of the ERWT3D adaptive 3-D seismic data storage framework. (a) Data input: the system analyzes data characteristics, access demand, and storage budget. (b) Adaptive representation construction: the planner evaluates LZ4 (lossless, axis-plane) and RZFP (error-bounded, axis-leaf) paths, selecting the optimal format. (c) Single-file organization: header, main representation with index, embedded X/Y/Z sections, and section directory. (d) Unified reader pipeline: slice request → section lookup → ordered I/O → decode and reorder. (e) Standard YZ/XZ/XY slice output.

**中文**: ERWT3D自适应三维地震数据存储框架概览。(a)数据输入：系统分析数据特征、访问需求和存储预算。(b)自适应表示构建：规划器评估LZ4(无损，轴平面)和RZFP(误差有界，轴叶)路径，选择最优格式。(c)单文件组织：头部、带索引的主表示、嵌入式X/Y/Z段和段目录。(d)统一读取器管线：切片请求→段查找→有序I/O→解码和重排。(e)标准YZ/XZ/XY切片输出。

---

## Fig.2 — Adaptive Selection and Physical Storage / 自适应选择与物理存储

**English**: (a) Physical storage ratio across three datasets. F3 amplitude and 20GB use LZ4 (lossless); F3 similarity uses RZFP (error-bounded lossy). Dashed line = raw baseline (ratio = 1). The small F3 amplitude volume incurs high relative overhead because fixed multi-axis representation cost dominates at small size. (b) Planner prediction vs measured physical ratio for 20GB. The planner predicts LZ4+XYZ at 2.142× (upper 2.937×) and RZFP+XYZ at 0.840× (upper 0.841×); measured values are 1.012× and 1.768× respectively. The planner correctly selects LZ4+XYZ.

**中文**: (a)三个数据集的物理存储比率。F3振幅和20GB使用LZ4(无损)；F3相似度使用RZFP(误差有界有损)。虚线=原始基线(比率=1)。F3振幅小数据集的相对开销较高，因为固定多轴表示成本在小体积下占主导。(b)20GB规划器预测与实测物理比率对比。规划器预测LZ4+XYZ为2.142×(上界2.937×)，RZFP+XYZ为0.840×(上界0.841×)；实测值分别为1.012×和1.768×。规划器正确选择LZ4+XYZ。

---

## Fig.3 — 20GB Multi-Axis Access / 20GB多轴访问

**English**: 20GB multi-axis access performance on SSD (cold Linux/WSL guest page-cache, n=5 runs, mean ± SD). (a) Random access: Raw vs ERWT3D for X and Y axes. Raw Z is excluded from quantitative comparison due to extreme cache sensitivity (CV > 90%). (b) Continuous access: similar pattern with smaller absolute differences. (c) ERWT3D axis balance: random and continuous access times across X/Y/Z axes show comparable order-of-magnitude performance, demonstrating balanced multi-axis access.

**中文**: 20GB在SSD上的多轴访问性能(冷Linux/WSL客户页缓存，n=5次运行，均值±标准差)。(a)随机访问：X和Y轴的原始数据与ERWT3D对比。原始Z轴因极端缓存敏感性(CV>90%)被排除。(b)连续访问：类似模式但绝对差异更小。(c)ERWT3D轴平衡：X/Y/Z轴的随机和连续访问时间在同一数量级，证明了均衡的多轴访问。

---

## Fig.4 — Codec Ablation on 20GB / 20GB编解码器消融

**English**: Codec ablation study on 20GB (SSD, n=5 runs). (a) Physical storage ratio: Auto LZ4 and Forced LZ4 achieve identical 1.012×, confirming planner consistency; Forced RZFP achieves 1.768×. (b) Composite access time (mean of 6 workloads): individual run points shown. LZ4 provides faster access than RZFP. (c) Random access by axis: LZ4 vs RZFP across X/Y/Z. RZFP is slower on all three axes. The planner's selection of LZ4 simultaneously provides lower storage overhead, lossless reconstruction, and faster multi-axis access.

**中文**: 20GB编解码器消融研究(SSD，n=5次运行)。(a)物理存储比率：Auto LZ4和Forced LZ4均为1.012×，确认规划器一致性；Forced RZFP为1.768×。(b)复合访问时间(6个工作负载均值)：显示单次运行点。LZ4比RZFP访问更快。(c)按轴随机访问：LZ4与RZFP在X/Y/Z上的对比。RZFP在三个轴上都更慢。规划器选择LZ4同时提供了更低存储开销、无损重建和更快的多轴访问。

---

## Fig.5 — F3 Real Seismic Data / F3真实地震数据

**English**: F3 dataset visualization and per-slice access latency. (a) F3 amplitude representative Z-slice (blue-white-red diverging colormap, ±99th percentile). (b) F3 similarity representative Z-slice (cividis sequential colormap). (c) F3 amplitude per-slice latency across X/Y/Z axes, random and continuous patterns (n=5 runs per group, box + scatter). Uses LZ4+XYZ (lossless). (d) F3 similarity per-slice latency. Uses RZFP+XYZ (lossy, max_rel < 0.001). Both datasets achieve stable millisecond-level slice access.

**中文**: F3数据集可视化与逐切片访问延迟。(a)F3振幅代表性Z切片(蓝-白-红发散色图，±99百分位)。(b)F3相似度代表性Z切片(cividis顺序色图)。(c)F3振幅在X/Y/Z轴上的逐切片延迟(每组n=5次运行，箱线图+散点)。使用LZ4+XYZ(无损)。(d)F3相似度逐切片延迟。使用RZFP+XYZ(有损，max_rel<0.001)。两个数据集均实现稳定的毫秒级切片访问。

---

## Fig.6 — RZFP Error-Bound Verification / RZFP误差界验证

**English**: RZFP error-bound verification for 20GB and F3 similarity datasets. (a) Maximum relative error: both datasets satisfy the contest bound of 1×10⁻³ (dashed red line). Violations = 0 for both. (b) NRMSE: 20GB: 9.12×10⁻⁵; F3 similarity: 1.91×10⁻⁴. Despite near-bound maximum errors, overall normalization error remains very low.

**中文**: 20GB和F3相似度数据集的RZFP误差界验证。(a)最大相对误差：两个数据集均满足竞赛界1×10⁻³(红色虚线)。两者违规数均为0。(b)NRMSE：20GB：9.12×10⁻⁵；F3相似度：1.91×10⁻⁴。尽管最大误差接近界值，整体归一化误差仍然很低。
