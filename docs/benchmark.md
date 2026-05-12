1: # ERWT3D Benchmark Document
2: 
3: ## Benchmark Method
4: 
5: The benchmark follows the competition scoring style:
6: 
7: 1. **Random Slice Test**: 100 random slice reads per axis (X, Y, Z), random indices with fixed seed
8: 2. **Continuous Slice Test**: 10 continuous adjacent slice reads per axis
9: 
10: Both include output write time. The competition composite time is:
11: 
12: ```
13: actual_composite_time = combined X/Y/Z random + continuous slice read/write time
14: ```
15: 
16: ## Scoring Formula
17: 
18: ```text
19: performance_score = (baseline_time / actual_composite_time) * 60
20: ```
21: 
22: ## Correctness Verification
23: 
24: All ERWT3D conversions are verified lossless using streaming random sampling (100k samples):
25: - `max_abs_error = 0`
26: - `max_rel_error = 0`
27: - `num_failed = 0`
28: - `passed = true`
29: 
30: See [erwt3d_verify](../tools/erwt3d_verify.cpp) for the streaming verify tool (memory-efficient, works on 50GB+ datasets).
31: 
32: ## Test Environment
33: 
34: - CPU: Intel i7-13700F, 24 threads (WSL2)
35: - RAM: 64 GB
36: - OS: Fedora Linux 43
37: - Compiler: g++ 15.2.1
38: 
39: ## Official 20G Data (small: 801x2405x2501, 18.0 GB raw)
40: 
41: ### Storage and Correctness
42: 
43: | Metric | Value |
44: |--------|-------|
45: | Raw size | 19,271,755,620 bytes (18.0 GB) |
46: | ERWT3D size | 20,719,862,016 bytes (19.3 GB) |
47: | Storage ratio | 1.075x |
48: | Conversion time | ~2 minutes |
49: | Correctness (100k samples) | max_abs_error=0, max_rel_error=0, num_failed=0, passed=true |
50: 
51: ### Full Benchmark Results (100 random + 10 continuous per axis)
52: 
53: | Config | T_x_rand | T_y_rand | T_z_rand | T_rand_avg | T_x_cont | T_y_cont | T_z_cont | T_cont_avg | T_total | Rand Balance | Cont Balance |
54: |--------|----------|----------|----------|------------|----------|----------|----------|------------|---------|-------------|--------------|
55: | **SB t1** | **373ms** | **149ms** | **199ms** | **240ms** | **423ms** | **179ms** | **170ms** | **257ms** | **249ms** | **2.50x** | **2.48x** |
56: | SB t8 | 718ms | 84ms | 125ms | 309ms | 349ms | 109ms | 107ms | 188ms | 249ms | 8.59x | 3.26x |
57: | SB t1 cache512 | 1119ms | 96ms | 94ms | 436ms | 308ms | 110ms | 113ms | 177ms | 306ms | 11.89x | 2.82x |
58: | SB t4 | 5240ms | 272ms | 182ms | 1898ms | 394ms | 94ms | 75ms | 188ms | 1043ms | 28.77x | 5.23x |
59: | Raw row-major | 6426ms | 151ms | 6ms | 2194ms | 2488ms | 9ms | 6ms | 834ms | 1514ms | 1093x | 402x |
60: | PRead (pread) | DNF | — | — | — | — | — | — | — | — | — | — |
61: 
62: **PRead backend times out on real data** (10+ minutes for just 20 random slices). 
63: The per-extent `pread()` syscall overhead (~389k calls per X-slice) makes it impractical.
64: 
65: **SB t1 is the recommended configuration** for the 20G dataset:
66: - Best overall T_total (249ms per average slice pair)
67: - Best axis balance (2.50x random, 2.48x continuous)
68: - Threads hurt X-axis performance due to mutex contention
69: - Cache adds overhead without improving superblock reads
70: 
71: **Actual composite time (100+10 full):** ~79.8 seconds
72: 
73: ## Official 50G Data (big: 2001x2201x3000, 50.4 GB raw)
74: 
75: ### Storage and Correctness
76: 
77: | Metric | Value |
78: |--------|-------|
79: | Raw size | 52,850,412,000 bytes (50.4 GB) |
80: | ERWT3D size | 55,197,040,896 bytes (52.6 GB) |
81: | Storage ratio | 1.044x |
82: | Conversion time | ~2 minutes |
83: | Correctness (100k samples) | max_abs_error=0, max_rel_error=0, num_failed=0, passed=true |
84: 
85: ### Full Benchmark Results (100 random + 10 continuous per axis)
86: 
87: | Config | T_x_rand | T_y_rand | T_z_rand | T_rand_avg | T_x_cont | T_y_cont | T_z_cont | T_cont_avg | T_total | Rand Balance | Cont Balance |
88: |--------|----------|----------|----------|------------|----------|----------|----------|------------|---------|-------------|--------------|
89: | **SB t1** | **2159ms** | **941ms** | **532ms** | **1210ms** | **299ms** | **513ms** | **177ms** | **330ms** | **770ms** | **4.06x** | **2.89x** |
90: | SB t8 | 2631ms | 1310ms | 768ms | 1570ms | 721ms | 384ms | 235ms | 447ms | 1008ms | 3.43x | 3.06x |
91: 
92: **SB t1 is the recommended configuration** for the 50G dataset:
93: - Best T_total (770ms per average slice pair, vs 1008ms for t8)
94: - Threads worsen performance across all axes
95: - Storage ratio is excellent (1.044x, well below 1.5x target)
96: 
97: **Actual composite time (100+10 full):** ~373 seconds (~6.2 minutes)
98: 
99: ## Backend Comparison Summary
100: 
101: | Backend | Storage Ratio | Syscall Profile | 20G Feasibility | 50G Feasibility | T_total (20G) | T_total (50G) |
102: |---------|---------------|-----------------|-----------------|------------------|---------------|---------------|
103: | **SB (superblock)** | 1.044x–1.075x | ~500–1650 preads/slice | Feasible (~80s) | Feasible (~373s) | **249ms** | **770ms** |
104: | PRead (extent) | Same | ~389k preads/slice | Impractical (DNF) | Impractical | DNF | DNF |
105: 
106: **SB is always better than pread.** SB reads whole 1 MiB superblocks (1 pread per grid cell), reducing syscall count by 100–800x compared to per-extent pread.
107: 
108: ## Final Recommended Command
109: 
110: For both 20G and 50G competition datasets, use single-threaded SB backend:
111: 
112: ```bash
113: ./build/erwt3d_bench \
114:   --input data.erwt3d \
115:   --output-dir final_bench \
116:   --random-count 100 \
117:   --continuous-count 10 \
118:   --threads 1 \
119:   --memory-limit-mb 8192 \
120:   --cache-mb 0 \
121:   --io-backend sb \
122:   --seed 20260511
123: ```
124: 
125: ### Key Settings
126: 
127: - `--io-backend sb`: Superblock I/O backend (mandatory for real data)
128: - `--threads 1`: Single-threaded is fastest (mutex contention outweighs parallelism)
129: - `--cache-mb 0`: Cache adds overhead without benefit for superblock reads
130: - `--memory-limit-mb 8192`: Works under 8GB (each superblock is ~1 MiB)
131: 
132: ## Thread Scaling Analysis
133: 
134: Threads consistently hurt performance on real data:
135: 
136: - **20G dataset**: t4 is 4.2x slower than t1 (5240ms vs 373ms X-random). Threads create mutex contention on the LRU cache and the thread pool.
137: - **50G dataset**: t8 is 1.3x slower than t1 (1008ms vs 770ms T_total). X-axis suffers most.
138: - Current implementation uses per-extent thread dispatch; a work-stealing model could improve this.
139: 
140: ## Cache Analysis
141: 
142: The LRU leaf cache (256B entries) does NOT help the SB backend:
143: - SB reads entire superblocks (1 MiB), not individual leaves
144: - Cache lookup adds mutex overhead on every read
145: - File system page cache already provides read-ahead for sequential access
146: - For 20G data: cache512 made T_total 23% worse (306ms vs 249ms)
147: 
148: ## Storage Ratio
149: 
150: Both datasets are well within the 1.5x target:
151: 
152: | Dataset | Ratio | Notes |
153: |---------|-------|-------|
154: | 20G (801x2405x2501) | 1.075x | Non-cubic dimensions add boundary superblock waste |
155: | 50G (2001x2201x3000) | 1.044x | Larger volume, less boundary overhead ratio |
156: | Synthetic 256³ | 1.000x | Perfectly aligned cubic volume |
157: 
158: ## Source Data
159: 
160: All results are derived from committed CSV evidence files in `docs/results/`:
161: 
162: | Table | Source CSV |
163: |-------|-----------|
164: | 20G summary | `docs/results/official20_summary.csv` |
165: | 50G summary | `docs/results/official50_summary.csv` |
166: | Backend comparison | `docs/results/official_backend_comparison.csv` |
167: | Storage & correctness | `docs/results/official_storage_correctness.csv` |
168: | Thread/cache matrix | `docs/results/official_thread_cache_matrix.csv` |
169: | Syscall profile | `docs/results/official_syscall_profile.csv` |
170: | Synthetic 256³ (legacy) | `docs/results/summary_table.csv`, `docs/results/io_backend_comparison.csv` |
171: 
172: ## One-Command Reproduction
173: 
174: ```bash
175: scripts/run_real_bench.sh data.raw NX NY NZ benchmarks/real
176: ```
177: 
178: ## Optimization Opportunities
179: 
180: 1. **I/O Optimization**: Direct I/O, io_uring for async I/O, or mmap could further reduce syscall overhead
181: 2. **Threading**: Work-stealing thread pool could improve parallelism for X/Y slices
182: 3. **Prefetching**: Predictive superblock prefetch could improve continuous slice speed
183: 4. **Memory-mapped files**: Could eliminate copy overhead entirely
