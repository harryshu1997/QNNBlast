# QBlast — Qualcomm Hexagon HTP 上的 CLBlast 式 Auto-Tuning 库
目标是基于 CLBlast 的设计思路，为 Qualcomm Hexagon HTP（NPU）写一个参数化 kernel + auto-tuner + 数据库 bake 的库。只针对 Qualcomm 一家硬件，不追求跨平台。

## 1. 项目目标与非目标

### 目标
- **Phase 1 MVP**：在 OnePlus 15（Snapdragon 8 Elite Gen 5）上，实现 **W4A16 GEMV** 的 auto-tuned kernel 库，在 LLaMA/Qwen-7B 的 decode 阶段典型 shape 上，相对 QNN SDK 默认 MatMul op 达到 1.5–3× 加速。
- 复现 CLBlast 的架构：**参数化 kernel 源码 + exhaustive/random tuner + JSON 结果 → C header → 静态链接进 .so + 运行时按 (device, shape) 查表**。
- 提供类 BLAS 的 C API（`qblast::GemvW4A16(...)`），方便后续被 ggml / llama.cpp 之类的上层集成。

### 非目标（Phase 1 明确不做）
- 跨 SoC（先只打 SD 8 Elite Gen 5 / Hexagon v79 或 v81；v75 以下忽略）。
- 跨精度（先只 W4A16；后续再加 INT8 GEMM、FP16 GEMM）。
- Fused attention、FlashAttention（后续 phase）。
- GPU / Adreno 后端（那层用 CLBlast 就行）。
- CPU 后端。

## 2. 硬件/软件目标

### 目标设备
- **OnePlus 15**，Snapdragon 8 Elite Gen 5，Android 15。
- **OnePlus 15 (CPH2749)**：Snapdragon 8 Elite Gen 5 (SM8850, codename canoe)，Android 16。
- ADB 设备 ID：`3C15AU002CL00000`（变更时更新）。脚本里用 `$ANDROID_DEVICE` 环境变量引用。
- Hexagon 版本：**v81**（SM8850 搭载），带 HMX 矩阵引擎 + VTCM。Phase 1 统一 `DSP_ARCH=v81`。

### 开发机环境
- Ubuntu 24.04 LTS，Linux x86_64。工作目录 `/home/myid/zs89458/`。
- **Conda 环境：`research`**（本项目的 Python 环境统一用这个，每开新 shell 先 `conda activate research`）。
- Android NDK r27c：`/home/myid/zs89458/android/android-ndk-r27c/`（可选；Hexagon SDK 6.5.0.1 里自带 r25c，大多数 workflow 用 SDK 自带的就够）。
- 参考实现：CLBlast 仓库 `/home/myid/zs89458/Documents/CLBlast/`（branch `research_dev`），已跑通 Android arm64 构建，[setup.md](/home/myid/zs89458/Documents/CLBlast/setup.md) 有完整的 build + adb push + on-device 执行流程，可抄。

## 2.5 关键架构决策：APK 包装（已实证，必读）

**Phase 1 实测结论：retail OnePlus 15 上 adb shell 永远访问不到 HTP，只能通过 Android APK 进程访问。** 这是底层约束，不是签名问题。

### 实证过程
1. 在 OnePlus 15（CPH2749，Android 16）上跑 Hexagon SDK 的 `examples/calculator`（CLI 版）失败：
   ```
   open_device_node failed for domain ID 3, sess ID 0 (errno 13, Permission denied)
   ```
2. `/dev/fastrpc-cdsp` 是 `system:system 0664 vendor_qdsp_device:s0`，adb shell（uid 2000，SELinux `shell` domain）**无权打开**——既不是签名/unsigned PD 问题，也不是 testsig 问题。
3. Qualcomm 自己的 `qnn-platform-validator` 从 adb shell 跑也失败（GPU 后端通过、DSP 后端失败 + 报 "use testsig" 错误）。
4. 改跑 SDK 的 **`examples/calculator_c++_apk`**（APK 版本）—— 安装 APK + push DSP skel 到 `/data/local/tmp/` + 启动 app + 自动化点击 Calculate 按钮 → **成功在 Hexagon v81 unsigned PD 上跑出正确结果**（sum of 0..99 = 4950）。

### 为什么 APK 能行
APK 进程跑在 SELinux `untrusted_app:s0` domain（或类似），vendor 策略对这个 domain grant 了 `vendor_qdsp_device` 访问权。这跟签名无关，是 Android 的 vendor partition 隔离 + SELinux app sandbox 设计——**只要进程是 app 起的，就能开 fastrpc 设备节点**。

### 对项目架构的影响
- **不能写 `samples/gemv_w4a16_bench.cpp` 这种 CLI 可执行**——必须包成 APK
- **Tuner 拆成 host 驱动 + on-device daemon 两半**：host 上跑 ARM 程序生成配置 / 收 JSON；on-device 一个 long-running APK service 接收命令、跑 kernel、回写结果
- 通信走 **本地文件 + Android Activity intent 或 broadcast**（用 `am broadcast` 触发 service），避免每次都重启 APK
- 调试时仍可用 `adb push` 把 DSP skel 推到 `/data/local/tmp/`，APK 在 `onCreate` 里把 `DSP_LIBRARY_PATH` 指过去即可——这条路径全验证过，照搬 SDK 的 `calculator_c++_apk/MainActivity.java` 的 `init()` 写法

### 启动 + 自动化技巧
- `adb install -r -t -g <apk>` 装 APK；如遇 `INSTALL_FAILED_VERIFICATION_FAILURE`，先关 verifier：
  ```
  adb shell settings put global verifier_verify_adb_installs 0
  adb shell settings put global package_verifier_enable 0
  ```
- OnePlus 还需要在开发者选项里打开 **"通过 USB 安装应用"** toggle（首次需手动）
- App UI 自动化：`adb shell uiautomator dump /sdcard/ui.xml` 取 XML → 解析按钮 bounds → `adb shell input tap X Y` 触发。Phase 1 直接 hardcode 坐标即可
- 输入 EditText 后**必须 `KEYCODE_BACK` 关掉软键盘**再点 Calculate 按钮（软键盘会遮按钮）
- 看结果：`adb shell uiautomator dump` 后读 `text="..."` 节点 / 或读 logcat 里 app 自己 logd 的 tag

### 备选：root 路径（保留但不用）
若将来要做 CLI 风格的快速迭代，OnePlus 15 (CPH2749) 国际版**支持 OEM bootloader 解锁**（标准 `fastboot flashing unlock`，会全盘清空 + Widevine L1 永久降级到 L3）。Phase 1 不走这条。

## 3. SDK 前置要求

**已安装并验证**（在 `/home/myid/zs89458/.zshrc` 配了 `qblast-env` 函数：`conda activate research && unset HEXAGON_SDK_ROOT && source $HEXAGON_SDK_ROOT/setup_sdk_env.source`）：

1. **Hexagon SDK 6.5.0.1**：`/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1/`
   - 工具链：`tools/HEXAGON_Tools/19.0.07/Tools/bin/hexagon-clang`（QuIC LLVM Hexagon Clang 19.0.07）
   - 关键例子：
     - `examples/calculator_c++_apk/` —— **Phase 1 起点**。Pre-built APK + DSP skel 都验证过能跑，APK 源码（Java + JNI + DSP skel）是 qblast Android 工程的模板
     - `examples/qhl_hvx/` —— HVX kernel 写法模板
     - `examples/calculator/` —— CLI 版（不能用，作为反例存档）
   - 环境脚本依赖 `python` 命令；脚本头部有 `if [ -n "$HEXAGON_SDK_ROOT" ]; then return; fi` 的 idempotent guard，所以 `.zshrc` 里**不能** `export HEXAGON_SDK_ROOT`，会让 source 早退。`qblast-env` 函数里有 `unset` 处理。
2. **QAIRT 2.45.0.260326**（AI Engine Direct / QNN 新品牌）：`/home/myid/zs89458/qairt/2.45.0.260326/`
   - 库路径含 v79/v81/v73/v69/v68/v66/v75 多版本 Hexagon lib，Phase 1 只用 v81。
   - `bin/aarch64-android/qnn-platform-validator` 可用作 sanity check（GPU 后端能直接 adb shell 跑过、DSP 后端从 shell 跑不过）
   - 另有旧版 `/opt/qcom/aistack/qairt/2.31.0.250130/` 忽略。
3. **ADB + Android platform tools**：`/usr/bin/adb`。
4. **Qualcomm Software Center / qsc-cli**：`/opt/qcom/softwarecenter/`，用于装 / 升级 SDK。CLI 包装 `/opt/qcom/softwarecenter/bin/qsc-cli/qsc-cli.sh`。
5. **Android Studio / Gradle**：**待装**。Phase 1 第一周需要装一个，用来生成 APK。也可以直接照搬 `examples/calculator_c++_apk/` 的 Gradle 项目模板，避免装 IDE，命令行跑 `./gradlew assembleDebug` 即可。
6. **scrcpy v3.x**：调试时屏幕镜像用。Ubuntu 24.04 apt 仓库版本太老（v1.25），用 GitHub 静态二进制：`/tmp/scrcpy-linux-x86_64-v3.3.4/scrcpy`

## 4. 与 CLBlast 的三个关键架构差异

照抄 CLBlast 的五层结构（kernels / tuning / database / routines / samples），但下面三点必须不同：

### 4.1 数据库 key 必须包含 shape bucket

CLBlast 最大的短板是"一个 device 一组参数"——对 tall/skinny GEMV 拉胯（见 CLBlast 仓库的 SGEMV benchmark：4096×4096 有 32 GB/s，11008×4096 只有 7.8 GB/s）。HTP 上 VTCM 切分对 shape 更敏感，必须分桶。

```cpp
// src/database/database_structure.hpp
struct Params {
    uint16_t tile_m, tile_n, tile_k;
    uint32_t vtcm_bytes_a, vtcm_bytes_b;
    uint8_t  n_hw_threads;
    uint8_t  dma_double_buf;
    uint8_t  quant_block;
    uint8_t  acc_precision;
    uint8_t  prefetch_depth;
    // ...
};

struct DatabaseEntry {
    std::string kernel_name;     // "gemv_w4a16", "gemm_int8", ...
    std::string soc_id;          // "SD8_ELITE_GEN5", "SD8_GEN3", ...
    std::string shape_bucket;    // "decode_n1_4k_4k", "decode_n1_4k_11k",
                                 // "decode_n1_lm_head_4k_32k",
                                 // "prefill_small", "prefill_large", ...
    Params      params;
};
```

运行时按 `(soc_id, kernel_name, shape_bucket)` 三元组查表，miss 时 fallback 到 `shape_bucket="default"`。

### 4.2 Tuner 是 host/device 分离的两个 binary

CLBlast 的 tuner 是一个进程，OpenCL 自己跨 host/device。HTP 不行：

- **`qblast_tuner_gemv` (ARM 可执行)**：生成配置 → 通过 QNN custom op API 把对应 kernel .so push 到 HTP → 执行 + 计时 + 收结果。
- **`libqblast_htp_skel_<variant_N>.so` (Hexagon ELF)**：每个配置编一份。先用 **pre-compile all variants** 方案（Week 5），后续可选升级到 JIT。

### 4.3 精度验证必须内置在 tuner 里

量化 kernel + 不同 accumulator 精度 + 不同 tile/DMA 顺序 **会**改变数值结果。每个候选配置跑完必须对 reference（FP32 matmul）算 max abs error；超过 tolerance（建议初始 `1e-2` 相对误差，后续按需收紧）**直接淘汰**，不看时间。

```
伪码：
for cfg in configurations:
    out = run_on_htp(cfg, A, x)
    if max_rel_err(out, ref_fp32) > TOL:
        reject(cfg); continue
    time_ms = measure(cfg, warmup=10, iters=100, take='median')
    record(cfg, time_ms)
```

## 5. 项目目录结构

```
qblast/                                 # 新仓库，建议 ~/Documents/QNNBlast/qblast/
├── CMakeLists.txt                      # 顶层：host 工具 + Android lib + Hexagon DSP 三条编译链
├── cmake/
│   ├── hexagon_toolchain.cmake         # Hexagon LLVM 交叉编译配置（DSP 侧）
│   └── android_toolchain.cmake         # NDK 交叉编译配置（ARM 侧 lib）
├── include/
│   ├── qblast.h                        # C API（外部用这个）
│   └── qblast.hpp                      # C++ API 可选
├── src/
│   ├── kernels/
│   │   └── hexagon/
│   │       ├── gemv_w4a16.c            # 参数化 HVX/HMX kernel 源码（DSP 侧）
│   │       ├── gemv_w4a16_common.h     # tile 循环、DMA 宏、量化 helper
│   │       ├── gemm_int8.c             # Phase 2
│   │       └── fused_matmul_silu.c     # Phase 3
│   ├── tuning/
│   │   ├── host/
│   │   │   ├── tuner_driver.cpp        # 跑在开发机的 host 端，生成配置 + 通过 adb 调度 daemon + 收 JSON
│   │   │   ├── configurations.cpp      # Cartesian product + constraint filter
│   │   │   ├── kernels/
│   │   │   │   ├── gemv_w4a16.hpp      # TunerSettings：参数空间 + 约束定义
│   │   │   │   └── gemm_int8.hpp
│   │   │   └── variant_builder.py      # 批量编 Hexagon variants（被 tuner_driver 调）
│   │   └── ondevice/                   # 跑在 APK 内（由 daemon Service 调用）
│   │       ├── runner_htp.cpp          # FastRPC + skel 加载 + 计时；JNI 暴露给 Java
│   │       ├── validator.cpp           # 在 ARM 上算 FP32 参考、对比 DSP 输出
│   │       └── runner_jni.cpp          # JNI bridge for Java Service
│   ├── database/
│   │   ├── database.cpp                # 查表逻辑（(soc_id, kernel, shape) → Params）
│   │   ├── database_structure.hpp
│   │   ├── shape_bucket.cpp            # 从 (M, N, K) → bucket 名字的映射
│   │   └── kernels/
│   │       └── gemv_w4a16/
│   │           ├── sd8e_gen5.hpp       # SM8850 / Hexagon v81 (OnePlus 15)，自动生成
│   │           └── sd8g3.hpp           # SM8650 / Hexagon v75 (OnePlus 12，备选验证机)
│   ├── routines/
│   │   ├── routine.cpp                 # 类 CLBlast Routine 基类
│   │   └── qgemv_w4a16.cpp             # dispatch：按 shape 选 kernel variant
│   └── api.cpp                         # C API 实现（连接 routine 层）
├── android/                            # Android Studio / Gradle 项目（生成 APK）
│   ├── build.gradle
│   ├── gradle/
│   ├── gradlew
│   ├── settings.gradle
│   └── app/
│       ├── build.gradle
│       └── src/main/
│           ├── AndroidManifest.xml
│           ├── java/com/qblast/tuner/
│           │   ├── MainActivity.java       # 触发 tuning 一键启动 / 显示状态
│           │   ├── TunerService.java       # long-running Service，接收 broadcast 命令
│           │   ├── BroadcastReceiver.java  # 监听 adb shell am broadcast 触发的命令
│           │   └── ResultLogger.java       # 写 /sdcard/qblast/results/*.json
│           ├── cpp/                        # JNI 入口，链接 src/tuning/ondevice 出的 .so
│           │   └── tuner_jni.cpp
│           └── res/                        # 极简 UI（一个状态文本 + 一个 trigger 按钮）
├── bench/                              # 用 APK 跑的端到端 benchmark Activity
│   └── llama_decode_shapes.cpp         # 通过 broadcast 给 daemon 派一组 shape，输出表格
├── scripts/
│   ├── database/
│   │   ├── database.py                 # JSON → C header（照抄 CLBlast）
│   │   └── json/                       # tune 结果落盘
│   ├── prep_device.sh                  # 锁频、关 thermal、关 verifier、装 APK 一条龙
│   ├── apk_build_install.sh            # ./gradlew assembleDebug && adb install -r
│   ├── push_skels.sh                   # 把 v81 / v75 variants 推到 /data/local/tmp/
│   ├── trigger_tune.sh                 # adb shell am broadcast -a com.qblast.TUNE …
│   ├── pull_results.sh                 # adb pull /sdcard/qblast/results/ ./scripts/database/json/
│   └── ui_automation.sh                # uiautomator dump + tap helpers（备用，trigger broadcast 失败时用）
└── docs/
    ├── architecture.md                 # 复述本文档的核心
    ├── tuning_space.md                 # 参数空间 + 约束的权威描述
    └── device_setup.md                 # OnePlus 15 / OP12 锁频、开 HTP 的步骤
```

### 数据流（运行时）

```
开发机 host                              OnePlus 15
─────────────                            ──────────────
tuner_driver  ──┐
  生成 cfg N    │
  编 variant   │
  push skel    ├──> adb push libgemv_v0042.so /data/local/tmp/
              │
  trigger     ├──> adb shell am broadcast -a com.qblast.TUNE \
                       --ei cfg_id 42 --es shape "4096_4096_n1"
              │                                        ↓
              │                                  TunerService (APK)
              │                                    .onReceive()
              │                                       ↓ JNI
              │                                  tuner_jni.cpp
              │                                       ↓ FastRPC
              │                                  libgemv_v0042.so on HTP
              │                                       ↓
              │                                  result + timing
              │                                       ↓
              │                                  /sdcard/qblast/results/v0042.json
  pull        ├──< adb pull
  parse <─────┘
  next cfg
```

## 6. Tuning 参数空间（Phase 1，GEMV W4A16）

### 核心参数（会被 tune）
```cpp
// src/tuning/kernels/gemv_w4a16.hpp
TunerSettings settings = {
    .parameters = {
        {"TILE_M",         {8, 16, 32, 64}},        // HMX M-dim tile
        {"TILE_K",         {64, 128, 256, 512}},    // K-dim 一次处理多少
        {"VTCM_A_BYTES",   {65536, 131072, 262144, 524288}},  // A 在 VTCM 占多少
        {"VTCM_X_BYTES",   {8192, 16384, 32768}},   // 激活 x 在 VTCM 占多少
        {"N_HW_THREADS",   {1, 2, 4, 6}},           // HTP 硬件线程数
        {"DMA_DOUBLE_BUF", {0, 1}},                 // 双缓冲
        {"PREFETCH_DEPTH", {0, 1, 2}},              // L2 prefetch 深度
        {"Q_BLOCK",        {32, 64, 128}},          // 量化 block 大小
        {"ACC_PRECISION",  {24, 32}},               // accumulator 精度 (bits)
    },
    .constraints = {
        // 只穷举合法组合
        [](const Config& c) {
            return c["VTCM_A_BYTES"] + c["VTCM_X_BYTES"] <= TOTAL_VTCM_BUDGET;
        },
        [](const Config& c) {
            return c["TILE_K"] % c["Q_BLOCK"] == 0;
        },
        [](const Config& c) {
            // HMX M-tile 必须满足硬件限制
            return c["TILE_M"] >= 8;
        },
    },
};
```

**预期搜索空间**：约束过滤后 ~500–2000 个合法配置。v79 HMX 的具体限制要查 Hexagon SDK 文档（`docs/QNN_HTP/`）。

### Shape buckets（Phase 1 先定这几个，后续补）
```
decode_n1_attn     : N=1, M∈{4096}, K∈{4096}          # Q/K/V/O projection
decode_n1_ffn_up   : N=1, M∈{11008, 14336}, K=4096    # FFN up/gate
decode_n1_ffn_down : N=1, M=4096, K∈{11008, 14336}    # FFN down
decode_n1_lm_head  : N=1, M∈{32000, 128256}, K=4096   # 输出投影
default            : 其他（fallback）
```

**命名原则**：bucket 名字编码 shape 范围，不是具体数值——这样换模型（LLaMA → Qwen → Gemma）不需要重 tune，只要新 shape 落在已有 bucket 就复用。

## 7. Phase 1 交付物

Phase 1 结束（约 12 周）产出：

```
bin/
├── qblast_tuner_gemv_w4a16              # ARM 可执行，单独 tune 一个 shape
├── qblast_bench_gemv_w4a16              # ARM 可执行，跑一组 shape 测速
└── qblast_bench_llama_decode            # ARM 可执行，跑端到端 LLaMA decode shape

lib/
├── libqblast.so                         # ARM shared lib，外部调用这个
└── libqblast_htp_skel.so                # Hexagon skel lib，内嵌所有 variant

include/
└── qblast.h                             # 公共 API

src/database/kernels/gemv_w4a16/
└── sd8e_gen5.hpp                        # 已 tune 的参数表

docs/
├── architecture.md
├── tuning_space.md
└── device_setup.md
```

### 验收指标
- 在 5 个 LLaMA-7B decode shape 上（见 CLBlast 仓库下之前的 sgemv benchmark），相对 QNN 默认 MatMul op：
  - 中位数加速 ≥ 2×
  - 最差 shape 加速 ≥ 1.5×
- 所有 tuned 配置数值误差 max relative error < 1e-2 vs FP32 reference
- `qblast_tuner` 单个 shape tune 时间 < 30 分钟（包括 variant 编译）

## 8. Week-by-Week 执行计划

### Week 0：环境 + 路径验证（**已完成**）
- [x] 装 Hexagon SDK 6.5.0.1 + QAIRT 2.45 via Qualcomm Software Center
- [x] OnePlus 15 (CPH2749) ADB 授权 + Developer Options 配好 + verifier 关掉
- [x] `qblast-env` shell 函数（在 `~/.zshrc`），一键激活 conda research + Hexagon SDK env
- [x] 跑通 SDK 自带 `examples/calculator_c++_apk` —— APK 装到 OnePlus 15 + push v81 DSP skel + 启动 app + 自动化点击 → 在 Hexagon v81 unsigned PD 上正确返回 sum=4950
- [x] 实证：CLI shell 路径不可用，APK 路径可用，**无需 root**
- [x] 备机：OnePlus 12 (CPH2583, SM8650, Hexagon v75) 也已就位，编了 v75 skel 备用

### Week 1：项目骨架 + APK 模板拷贝
- [ ] 建 qblast 仓库（`~/Documents/QNNBlast/qblast/`），按第 5 节目录结构
- [ ] 把 SDK 的 `examples/calculator_c++_apk/` 整个拷到 `qblast/android/`，重命名 package 为 `com.qblast.tuner`
- [ ] 改 `MainActivity.java`：保留 `init("/data/local/tmp")` 的 DSP_LIBRARY_PATH 设置；把 EditText/Button UI 替换成单 TextView + "Trigger Tune" Button
- [ ] 加 `TunerService.java`（IntentService 或 ForegroundService）+ BroadcastReceiver，监听 `com.qblast.TUNE` action，extras 带 `cfg_id` / `shape`
- [ ] `scripts/apk_build_install.sh`：`./gradlew assembleDebug && adb install -r app-debug.apk`
- [ ] `scripts/prep_device.sh`：调 `settings put global verifier_*` 关 verifier、锁 CPU 大核 governor（HTP 频率锁需 root 或 QNN Performance API，先做 best-effort 版）
- [ ] `scripts/trigger_tune.sh`：`adb shell am broadcast -a com.qblast.TUNE …`

**产出**：装一个空壳 APK，能通过 `am broadcast` 触发 service，service 在 logcat 里打印收到的参数。

### Week 2：Baseline GEMV W4A16（单配置、不 tune）
- [ ] 在 `src/kernels/hexagon/gemv_w4a16.c` 写**单配置、不参数化**的 baseline（`TILE_M=16, TILE_K=128, N_HW_THREADS=2, Q_BLOCK=64`）。HVX load/store + HMX matmul + W4 dequant + FP16 accumulate
- [ ] 写 `runner_jni.cpp`：JNI 包装，从 Java 接收 (M, K, A_handle, x_handle, out_handle)，FastRPC 调用 DSP skel，记录 cl_event-style timing
- [ ] 写 `validator.cpp`：在 ARM 上算 FP32 reference，对比 DSP FP16 输出（max relative error < 1e-2）
- [ ] APK Service 接收 broadcast 参数 → 调 JNI runner → 写结果到 `/sdcard/qblast/results/{cfg_id}.json`
- [ ] `scripts/pull_results.sh`：拉回 host

**产出**：`./scripts/trigger_tune.sh --shape 4096_4096_n1 --cfg-id 0`，能在 ~10s 内拿到 baseline 的 timing + 数值正确性确认。

### Week 3：参数化 kernel + 手动跑多配置
- [ ] 把 `gemv_w4a16.c` 改成参数化（`#define TILE_M`、`#define VTCM_A_BYTES` 等）
- [ ] 写 `variant_builder.py`（host 上跑，调 Hexagon LLVM 批量编出多个 `.so`，命名 `libgemv_v{cfg_id}.so`）
- [ ] 手动写 10 个配置的 JSON、编出来
- [ ] `scripts/push_skels.sh` 一次性推全部 variant 到 `/data/local/tmp/qblast_variants/`
- [ ] APK Service 在收到 `cfg_id` 后用 `dlopen("libgemv_v{cfg_id}.so")` 切换 variant
- [ ] 手动 trigger 每个 cfg、看哪些数值对、看相对速度

**产出**：手动确认参数化 kernel 数值仍正确；不同参数 cfg 之间性能差异可见。

### Week 4–5：Tuner 框架 + 自动化
- [ ] 写 `src/tuning/host/configurations.cpp`（照抄 CLBlast 的 `PopulateConfigurations`）
- [ ] 写 `src/tuning/host/tuner_driver.cpp`：生成配置 → 调 `variant_builder.py` → 推 skel → adb broadcast trigger → adb pull JSON → 收集
- [ ] 处理 variant 数量爆炸：先 **random sampling (fraction=0.1)** 减到 ~200 个跑通
- [ ] APK Service 增加 batch mode：一次 broadcast 跑一组 cfg_id，全跑完一起 dump

**产出**：`./qblast_tuner_driver -m 4096 -k 4096` 端到端跑 ~200 个配置 → 筛最优 → 输出 `tune_4096_4096_n1.json`，整套不需要人工干预（除了第一次装 APK 时的物理点击）。

### Week 6：Database pipeline（host 端）
- [ ] 抄 CLBlast 的 `scripts/database/database.py`，改成读 QBlast JSON 格式
- [ ] 定义 `DatabaseEntry` 结构（含 shape_bucket 字段）
- [ ] 实现 JSON → `src/database/kernels/gemv_w4a16/sd8e_gen5.hpp` 自动生成
- [ ] 实现 `src/database/database.cpp` 的查表逻辑（含 fallback 到 default bucket）
- [ ] 把生成的 header 静态链接进 `libqblast.so`（这个 .so 跑在 ARM/APK 内）

**产出**：`libqblast.so` 启动时加载 database，APK 内调 `qblast::GemvW4A16(...)` 能按 shape 自动选 cfg_id 加载对应 variant。

### Week 7–8：Routine 层 + C API + 集成 benchmark
- [ ] 实现 `src/api.cpp` + `include/qblast.h`
- [ ] Routine 层做 shape → bucket 映射（`src/database/shape_bucket.cpp`）
- [ ] 写集成测试：从 C API 调用 vs 直接调 variant，结果一致
- [ ] `bench/llama_decode_shapes.cpp`（也跑在 APK 里）：通过 broadcast 派一组 5 个 shape，输出 markdown 表格到 `/sdcard/qblast/bench.md`

**产出**：对外 API 可用，端到端跑通；能在 OnePlus 15 上跑 LLaMA-7B 5 个 decode shape 的对比表格。

### Week 9–10：覆盖多 shape bucket + tune
- [ ] 为每个 shape bucket 跑一次 `tuner_driver`（5 个 shape × ~30 min/shape × ~200 cfg = ~2.5h/shape，全 bucket 一晚跑完）
- [ ] 把结果合进 database，重编 `.so`、重打 APK
- [ ] 对比 tune 前后数字 vs QNN 默认 MatMul op
- [ ] 如果加速不达标（< 1.5×）：分析 bottleneck（VTCM 没吃满 / DMA 覆盖不了计算 / HMX tile shape 不对）

**产出**：第一版 tuned database，达验收指标（中位数 ≥ 2× over QNN 默认 MatMul）。

### Week 11：鲁棒性 + 文档
- [ ] `docs/architecture.md`、`docs/tuning_space.md`、`docs/device_setup.md`（设备解锁可选 + APK 安装流程 + verifier toggle）
- [ ] 写 unit tests（kernel 输出 vs reference、database 查表、shape bucket 映射）
- [ ] 清理错误处理：FastRPC 返回错误、variant .so 加载失败、精度验证失败的处理路径
- [ ] `scripts/prep_device.sh` 完善
- [ ] APK Service 加重试、超时、batch checkpoint（中途崩了能续跑）

### Week 12：收尾 + Phase 2 规划
- [ ] 跑一次完整的验收 benchmark，记录数字
- [ ] Code review + 重构一轮
- [ ] 写 Phase 2 proposal：加 INT8 GEMM？加 fused attention？接入 ggml-hexagon 后端（`/home/myid/zs89458/Documents/llama.cpp/ggml/src/ggml-hexagon/` 已存在，可作为集成目标）？

## 9. 已知陷阱（必读）

1. **Hexagon SDK 学习曲线陡**。HVX 向量是 1024-bit 定长，不像 NEON/AVX 那么直观；HMX tile shape 是硬件固定的（不是任意的）；文档分散在 SDK 里多份 PDF。**Week 1 要留 3–4 天读 `HVX_Quick_Reference.pdf` 和 HMX 章节**，硬读，没有捷径。

2. **FastRPC 延迟不可忽视**。每次 ARM → HTP 调用 ~100 µs overhead。对 GEMV（decode 单 token）这个开销可能和计算同数量级。**小 shape 的 tune 结果要包含"启动开销"的考量**——不要光看 event 时间。如果要做大规模 inference，考虑 QNN graph execute 模式（一次 RPC 跑多 op）。

3. **QNN custom op 注册链路繁琐**。op package 定义 → QNN converter → context binary 生成，每步有陷阱。**先跟着 SDK 的 `examples/CustomOp` 完整跑一遍**，不要自己发明流程。

4. **真机测速不稳定**。Android 有 thermal throttle、DVFS、big.LITTLE 调度。必须锁频 + 预热 + 多次取中位数。`scripts/prep_device.sh` 至少包含：
   ```bash
   # 锁 CPU 到大核固定频率
   echo performance > /sys/devices/system/cpu/cpu7/cpufreq/scaling_governor
   # 禁用 small/middle core（让 tuner 跑在稳定频率的大核）
   for i in 0 1 2 3; do echo 0 > /sys/devices/system/cpu/cpu$i/online; done
   # HTP 锁频（需要 root 或特殊路径；可能要用 QNN Performance API）
   ```

5. **v79 HMX 文档不全**。v75 之前的资料会误导。**只信 Hexagon SDK 6.x 附带的 PDF 和 header 注释**，以及 Qualcomm 开发者论坛上 2025 年后的帖子。

6. **Variant 数量爆炸**。全参数 cartesian product 可能 10000+ 配置，每个编一个 .so 会挤爆存储 + 编译要几小时。**Phase 1 用 `--fraction` 随机采样 10%–20% + 强约束过滤**，先跑通流程。

7. **OpenCL 可以作为 sanity check 通道**。遇到 HTP kernel 可疑结果，用相同的 (M, N, K, data) 在 Adreno GPU 跑 CLBlast 的 SGEMV，两边对数——能快速隔离是数值 bug 还是量化精度问题。

## 10. 集成到上层（Phase 2+）

Phase 1 完成后，API 设计要考虑能无缝接入：

- **ggml-opencl 已经有 CLBlast 分支** 的模式（`#ifdef GGML_OPENCL_USE_CLBLAST`）——Phase 2 可以在 ggml-hexagon/ggml-qnn 后端里加一个 `#ifdef GGML_USE_QBLAST` 分支，改动极小。参考 `/home/myid/zs89458/Documents/llm.cpp/ggml/src/ggml-opencl/clblast-ext.h` 的 wrapper 风格。
- **C API 签名尽量对齐 CLBlast**：`qblast::Gemv(Layout, Transpose, m, n, alpha, A, a_offset, a_ld, X, x_offset, x_inc, beta, Y, y_offset, y_inc, context, event*)` —— 这样已有调用方切后端只要换头文件和链接。

## 11. 参考代码库

- **`/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1/examples/calculator_c++_apk/`** —— **Phase 1 的 APK 模板**，已实测可行。包含：
  - `app/src/main/java/com/example/calculator/MainActivity.java` —— `init("/data/local/tmp")` 设置 `DSP_LIBRARY_PATH` 的官方写法，照抄
  - `app/src/main/cpp/calculator-jni.cpp` —— JNI bridge 模板
  - `app/build.gradle`、`build.gradle`（顶层）、`settings.gradle` —— Gradle 配置
  - `dsp/calculator_imp.c` + `dsp/CMakeLists.txt` —— DSP 侧 skel 编法
  - `calculator_c++.apk` —— 预编 APK，可作为环境验证用
- **`/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1/examples/`** 其他必看子目录：
  - `qhl_hvx/` —— HVX kernel 写法 + 性能优化模板
  - `multithreading/` —— HTP 多硬件线程使用范式
  - `dspqueue/` —— 给 DSP 派批量任务的 queue API（tuner batch mode 可参考）
- **`/home/myid/zs89458/Documents/CLBlast/`**（branch `research_dev`）——架构抄它，尤其：
  - `CMakeLists.txt`（SAMPLE/TUNER/DATABASE 的 CMake 组织）
  - `src/tuning/tuning.hpp`、`src/tuning/configurations.cpp`（tuner 骨架，host 侧逻辑可直接迁移）
  - `src/database/database.cpp`、`src/database/database_structure.hpp`（查表结构）
  - `scripts/database/database.py`（JSON → C header 的 codegen）
  - `samples/sgemm.cpp`、`samples/sgemv.cpp`（benchmark 工具范式，对应 qblast 的 `bench/` 内 Activity）
  - `setup.md`（Android build + adb push + on-device 执行流程）
- **`/home/myid/zs89458/Documents/llm.cpp/ggml/src/ggml-opencl/clblast-ext.h`**——已有的 CLBlast 集成 wrapper 风格，将来 QBlast 集成 ggml 时参考。
- **`/home/myid/zs89458/Documents/llama.cpp/ggml/src/ggml-hexagon/`** —— 已有的 ggml hexagon 后端（在做中），Phase 2 集成目标。

## 12. 下一个 session 开始时的检查清单

读完本文档后，下一个 session 应该：

1. **环境**：开新终端跑 `qblast-env`（已在 `.zshrc` 里），应该输出 "Setting up the Hexagon SDK environment locally" + qaic 编译输出。
2. **设备**：`adb devices` 看到 `3C15AU002CL00000  device`（OnePlus 15）。如果 unauthorized，在手机上重授权并勾 "Always allow"。备机 `5ae7a43d` 是 OnePlus 12 (CPH2583, v75)，可选。
3. **Sanity check**：跑 `examples/calculator_c++_apk` 的 APK 路径，确认 sum=4950（详细命令在第 2.5 节）。这一步只在第一次或换设备时做。
4. **建仓库**：`mkdir -p ~/Documents/QNNBlast/qblast`，按第 5 节目录结构建骨架。**Week 1 的第一步是 `cp -r $HEXAGON_SDK_ROOT/examples/calculator_c++_apk/* ~/Documents/QNNBlast/qblast/android/`** 然后改 package name + UI。
5. **如果 OnePlus 15 上 verifier 又开了**（手机重启或 ColorOS 自动恢复），重新跑：
   ```
   adb shell settings put global verifier_verify_adb_installs 0
   adb shell settings put global package_verifier_enable 0
   ```

## 13. 本文档未覆盖、需要开工后现场决定的

- **具体的 HVX/HMX intrinsic 使用方式**——必须读 SDK 文档（`docs/QNN_HTP/`、`docs/HVX/`）后决定，本文档只给方向。
- **VTCM 大小到底是多少**——v81 上**实测**才能确定（先 `qnn-platform-validator --backend HTP --testBackend` 在 APK 内跑出来）。文档说 SD 8 Elite Gen 5 的 VTCM 通常 8 MB，但 OEM 可能裁成 4 MB。
- **Tolerance 具体取值**（1e-2 只是初始建议）——跑起来看 workload 可接受度再调。
- **Shape bucket 边界怎么切**——Phase 1 跑完一版 tune 数据后，看 heatmap 决定最终分桶策略。
- **APK 内 daemon 的进程模型**：到底是 `IntentService`（单次 broadcast 启动）还是 `ForegroundService`（一直驻留）？前者每次冷启 ~1s 但稳定；后者 ~0 启动延迟但容易被 OS 杀。Phase 1 先用 ForegroundService + START_STICKY，配 `notification` 占位避免被回收。
- **HTP 锁频**：APK 进程能否调 QNN Performance API 锁住 HTP 频率？不行的话只能锁 CPU 大核 + 多次取中位数缓解抖动。
- **APK 签名**：debug keystore 够 Phase 1 用；Phase 2 如果要分发或挂 ggml 上层调用，需要正式 keystore + Play Console 签名。
