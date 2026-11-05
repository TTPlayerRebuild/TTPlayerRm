# Cook 状态边界与剩余数值差异（2026-11-05）

本轮继续验证播放解码替代，复用此前原 `cook.dll` 生成的 34 个预设样本。预设覆盖不是重建编码器的证明；没有新增 RA 编码或导出接口替身，也没有给生产代码增加旧 DLL 依赖。

## 范围与证据身份

- 原 `RealCodec/cook.dll` SHA-256：`7345fb0678e8f0b20427d6fed5d53ca0642cb19c0a019b81e9101dbca2bcc723`。
- 初始被测 Reader `2026.10.29p1` SHA-256：`fb6fa95e872f2571b5b9824f8adb7e70ead2fb48262fce2e3db9bc7812decedd`。
- 首个修复候选 Reader `2026.11.05` SHA-256：`f66cd7e1974ce3c5c3e3fde26a5f736917f8948fa6adaab16bb126674f667426`；本机 43 组全部通过。最终构建在共享原始 `.ra` 尾部边界修正后重新验收，身份与系统结果见 [统一验收](RM_COMPLETION_20261105.md)。
- 直测链接的 FFmpeg `avcodec.lib` SHA-256：`51a026fe9f88622918ebdc37deb62e774cc8cec8ff85f71b135f02a7dd339582`。
- 所有新增探针、可执行程序、输入样本、原 DLL、数值输出和日志仅保存在工作区私有目录 `rebuild/tests/rm_completion_20261105/cook/`，不进入 Actions 或发布包。
- 复用的静态证据为 [前轮补噪恢复](COOK_NOISE_PARITY_20261029.md) 和 [结束边界与 34 预设验证](COOK_END_OF_STREAM_20261029P1.md) 所列 JSON；本轮未重新反编译。

## Reader 状态验证

新增 43 组基线检查中，40 组通过，3 组暴露单包截断被当作 EOF 的问题。

34 个合法预设均逐字节通过以下检查，包含 mono、独立 stereo、joint stereo、5.0 和 5.1：

1. 完整读取后 `Seek(0)` 再读。测试流标为 remote，禁用 PCM history，强制实际销毁并重建解码器，不能靠缓存掩盖状态没有重置。
2. 释放 Reader、重新创建并完整读取。
3. 两个 Reader 以不同缓冲长度交错读取；其中一个在第 18 轮重置，两个最终输出分别与独立基线完全一致。

另外 9 组异常测试包括 5 个截断包、两个坏多声道 subpacket 长度表、两个非法多声道类型头。已拒绝的异常均验证重复调用清空输出、错误稳定、重新定位后的前缀一致、另一个健康实例不受影响，以及创建新 Reader 后恢复合法文件的完整 PCM。Reader ABI 没有 Close/Reset；这里的 reopen 指释放并重新创建，流内重置指 `Seek(0)`。

### 已复现缺口

`flavor0`、`flavor9`、`flavor17` 在第 11 个 RM 包仅缺最后一个字节，包头仍声明完整长度。demuxer 输出 `Failed to fully read block`，Reader 却返回正常 `S_FALSE`。分别覆盖单声道、独立双声道、联合立体声。私有文件为 `stage/flavor{0,9,17}-truncated.rm`，初始日志为 `reader-34.log` 至 `reader-36.log`。

5.0 / 5.1 的相同截断方式碰巧触发后续 Cook quantizer 检查，返回 `0x8007000D`；不能由此推断容器边界已正确校验。修复应验证实际输入是否满足包头声明的长度，不能依赖某一编码模式最终是否遇到非法量化值。此项由共享 Reader/后端边界工作统一处理。

### 2026.11.05 修复后复验

生产构建已启用严格 RM 短包检测。用其重建平铺 `stage`，运行 `run.py after` 和实际 `stage/run.cmd`，均得到 **43 组、0 失败**。`after-results.json`、`after-00.log` 至 `after-42.log`、`modern-reader-after-stage.log` 保存此次结果；原 `reader-*` 基线及 3 个失败保持不变。

全部 5 个截断文件和 2 个损坏长度表文件现在稳定返回 `0x8007000D`，不会变成正常 EOF；第一次错误后连续 3 次 Read 均保留该错误且 PCM 长度为零。每个文件 `Seek(0)` 后再读仍得到相同的有效前缀及错误，另一个健康实例和之后新建的合法 Reader 均与完整基线一致。

`after-prefix-checks.json` 另外核对了有效输出长度：10 个完整编码包扣除 2 个预热帧，精确为 8 个解码帧，没有由截断包补零生成额外 PCM：

| 预设 | 模式 | 有效 PCM 字节数 |
| --- | --- | ---: |
| 0 | mono，截断 | 4,096 |
| 9 | 独立 stereo，截断 | 8,192 |
| 17 | joint stereo，截断 | 16,384 |
| 30 | 5.0，截断 / 长度表损坏 | 81,920 |
| 31 | 5.1，截断 / 长度表损坏 | 98,304 |

其字节内容也都与完整合法文件相同位置的前缀逐字节一致。两种非法多声道头仍连续 3 次初始化失败，失败后的 Format 指针和 Read 长度清空；34 个合法预设的重置、重新打开和实例隔离继续通过。

## 原库和当前核心的直接状态对照

`direct.cpp` 对每个预设解交错得到 100 个合法编码包，并通过原库公开接口调用。34 组均验证：

- 原库 `RAFreeDecoder` 后在同一 codec wrapper 上 `RAInitDecoder`，输出与初始运行完全一致。
- 当前核心销毁并重新创建 context，输出 float 逐字节一致。
- 两个原库实例和两个当前核心实例交错运行；重置其中一个不影响另一个。
- 在 8 个完整包后插入长 1 字节或 `block_align - 1` 字节的输入，再接原合法第 9 包。原库和当前核心均成功返回且不产生新 PCM；完整后续输出与未插入短输入时相同，说明这些输入没有推进被测状态。该观察不表示容器应把声明长度不符的物理截断当作正常 EOF。

另单独记录当前核心 `avcodec_flush_buffers()` 后重播：34 个预设均不等于新 context 的基线。Cook 没有实现 flush 回调，因此该通用调用不等价于完整重置。当前 Reader 用销毁/重建完成需要重新解码的定位，34 组已验证这条实际路径；本轮没有为没有调用者的内部路径增加生产补丁。

原库 `RAFlush` 也不是重置 API。已保存 `60F11560` 伪代码表明其执行 3 次核心处理并恢复预热计数，不能推断它重置随机状态；它仍按前轮证据用于取得未交付的 pending PCM。本轮数值参考沿用“原正常输出 + 一个已证明独立于后继内容的 pending 帧”，没有删尾音。

## 1 LSB 的定位结果

本轮重新捕获全部 34 预设的当前 FLTP 输出及原库 PCM16，比较 5,745,152 个声道采样值，逐项复现前轮的 5,073 个 ±1 LSB 差异。没有增益拟合、偏置拟合或时间对齐搜索。

| PCM 转换规则 | 不相同采样值 | 最大误差 / LSB | RMS / LSB |
| --- | ---: | ---: | ---: |
| 当前 nearest-even | 5,073 | 1 | 0.0297154 |
| `floor(x + 0.5)` | 5,574 | 1 | 0.0311482 |
| 半整数远离零 | 5,779 | 1 | 0.0317158 |
| 向零截断 | 2,793,317 | 1 | 0.6972834 |
| SIPR 式 `trunc(x + 0.5)` | 2,813,590 | 2 | 0.7000957 |

`x` 为当前 float 精确乘以 32768，所有规则都使用相同 PCM16 饱和范围。当前差异正向 2,594 个、负向 2,479 个；1,427 个正好落在半整数，3,646 个不是半整数。全部差异离半整数的距离不超过 `0.005859375` LSB。

更强的反例来自完全相同的当前 float 值，却对应不同的原版整数：

| 当前 float × 32768 | 第一处原 PCM | 第二处原 PCM |
| ---: | --- | --- |
| 12630.5029296875 | flavor4，采样索引 30386：12630 | flavor5，索引 92616：12631 |
| -9589.5 | flavor5，索引 32454：-9590 | flavor6，索引 6380：-9589 |
| 8038.5009765625 | flavor4，索引 87842：8038 | flavor6，索引 14582：8039 |

索引为从 0 开始的交错声道采样值索引。`quantization-results.json` 保留 20 个此类反例及全部按预设统计。相同输入浮点数不能通过同一个无状态标量量化函数映射成两个不同整数，因此不能靠统一调整最终舍入规则消除全部差异。

证据支持“差异已存在于最终 PCM 量化之前”的判断；目前没有证明具体是某一个 x87 中间结果、MDCT 步骤、窗口系数或乘加顺序。没有足够依据写出通用精度补丁。本轮保留原有 nearest-even 和 1 LSB 门限，不添加样本、预设、位置或 PCM 值特判。

## 复现

私有目录中的 `build.py` 使用现有 x86 MSVC、VC-LTL 5.1 和 YY-Thunks 构建；EXE 的 PE subsystem 为 5.01。原库所需 `msvcr71.dll` 仅放入私有直测目录。所有被测进程调用 `SetErrorMode` 禁止崩溃提示窗口，每个子进程有 30 秒超时，不启动播放器。

```text
python rebuild/tests/rm_completion_20261105/cook/build.py
python rebuild/tests/rm_completion_20261105/cook/prepare.py [candidate-ttp_rm.dll]
python rebuild/tests/rm_completion_20261105/cook/run.py [result-label]
python rebuild/tests/rm_completion_20261105/cook/run.py direct
python rebuild/tests/rm_completion_20261105/cook/quantization.py
```

复制平铺的 `stage` 到 XP / Win7 可分别运行：

- `run.cmd`：43 组 Reader 边界验收。
- `run-direct.cmd`：34 组原库/当前核心直接对照。输出的 float/PCM 留在本地目录。

最终 DLL 的本轮 XP / Win7 执行记录为统一目录下的 `cook-final-xp.log`、`cook-final-win7.log`，本机为 `cook-final-win11.log`；详见统一验收。低层 `direct.exe` 仍链接上文列出的旧核心快照，只用于状态和数值研究，不混入最终 DLL 计数。`reader-*` 是发现缺口时的基线记录。
