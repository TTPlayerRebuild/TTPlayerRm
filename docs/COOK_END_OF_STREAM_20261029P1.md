# Cook 结束边界、完整 PCM 与多声道验收（2026.10.29p1）

## 结论

2026.10.29 的 Cook 随机补噪修正继续有效。本轮把原来只覆盖 4 个联合立体声历史文件的验证，扩展到原 `cook.dll` 编码器的 34 个有效预设，包括单声道、独立双声道、联合立体声、5.0 和 5.1。完整 PCM 的差异均不超过 1 个 PCM16 最低有效位。

原版与重建版末尾相差一个合成帧的原因已经明确：原 `cook.dll` 比 FFmpeg 多一个输出管线延迟帧，原 `ttp_rm.dll` 在容器结束时没有排出它。这个帧可能含有正常音乐，不能以相同长度为目标删除，也不能根据全零内容裁切。重建版继续保留已经解码的完整 PCM；本轮没有增加丢弃最后一帧的兼容逻辑。

同时修复多声道格式声明的缺口：RM 容器只提供声道数量，Cook 解码器初始化后才解析出明确的扬声器位置。原重建 Reader 提前读取了容器参数，5.0 / 5.1 的 `WAVEFORMATEXTENSIBLE.dwChannelMask` 因而为零。主任务已将正确布局同步和失败清理并入共享 Reader，本轮测试验证了实际 mask 与延迟打开的一致性。

## 二进制依据

原 `RealCodec/cook.dll` SHA-256：

```text
7345fb0678e8f0b20427d6fed5d53ca0642cb19c0a019b81e9101dbca2bcc723
```

| 地址 | 行为 | 本轮意义 |
| --- | --- | --- |
| `60F116D0` | `RAInitDecoder` 的实现将核心报告的预热帧数存入对象 `+0x24`，计算 `+0x20` 待跳过 PCM 字节数 | 运行中 34 个有效预设均报告 3 个帧；不是根据 PCM 值推测 |
| `60F11260` | `RADecode` 逐编码帧调用核心，再按 `+0x20` 跳过起始输出 | 证明额外时延发生在原 codec 层 |
| `60F11560` | `RAFlush` 调用核心并返回 3 个帧，随后恢复预热计数 | 第一个帧是尚未交付的 PCM；其余帧不能一概当成容器里的有效音频 |
| `60601DAB` | 原 `ttp_rm.dll` 的解码包装器按编码帧大小调用 `RADecode` | 容器解交错与 codec 输出是不同层 |
| `60608556` | 原 Reader 在底层读包返回 0 时直接返回 `S_FALSE` | 正常 EOF 路径没有调用已保存的 `RAFlush` 指针 |
| `60F15530` | `RAInitEncoder` 从预设表初始化编码器，返回输入采样值数与编码字节数 | 可以用原编码器生成其他合法编码模式的测试文件 |
| `60F11900` | `RAGetFlavorProperty` 返回预设 PCM 描述、比特率及 Cook 类型头等 | 生成文件使用原库返回的真实参数，而非修改已有立体声文件的声道字段 |

REA/Ghidra Evidence：

- 原解码包装器：`ev_15bd92d0cb09354c3b97881b380fb24516971f44f77cb1b6cfbd440fa6a492c9`。
- 原 Reader EOF：`ev_33e3ed01e525000051f14ff1eef026dd2f17cc5b4b2700ae353b2c61cd1f2ee9`。
- 原 Flush：`ev_04162a5a1409f651d31cd17eea24a4e17681fa2fc714f4f0c6853506757d8225`。
- 原编码器初始化：`ev_8cc0727fea53fbdefb16a9f7dfed90519dbe1078d2b86cd8b2049c080c380fb2`。
- 预设属性：`ev_2649fa8f674b3af18c73a77be8ae630faeb3e73e2dd446b89b73a4a520dc3c5d`。

工具输出是反编译证据，不是原始源码。原文件保持不变，探针通过正常导出接口调用原库。`RADecode` 等导出在 x86 下使用 `stdcall`，不能按 `cdecl` 声明。

## 一个帧差异的实验

同一个历史 Cook32 文件，解交错后有 720 个编码包，每包对应每声道 1024 个采样值：

| 路径 | 起始跳过帧 | 实际输出采样帧 | EOF drain 输出 |
| --- | ---: | ---: | ---: |
| 直接调用原 `cook.dll` | 3 | 734,208 | 原正常 Reader 不调用 Flush |
| 原 `ttp_rm.dll` Reader | 3 | 734,208 | 0 |
| 当前 FFmpeg / 重建版 | 2 | 735,232 | 0 |

直接调用原 codec 所得的正常输出与原 Reader 的完整输出逐字节相同；SHA-256 为 `bc08d0d12fb4fa7813c940bab1a329398e689df864d6c9fe7b92a89a1c8bce3f`。FFmpeg Cook 未声明 `AV_CODEC_CAP_DELAY`，本轮运行也确认发送空包 drain 没有产生额外帧。因此最后一个帧不是现代 drain 意外制造的。

在输入第 1、2、3、4、5、10、72、100、360、719 个编码包后停止，对原 codec 分别再提供三种不同的合法后继：实际下一包、重复上一包、文件首包。30 次实验均通过：同一截点下，三种后继交付的第一个 PCM 帧逐字节相同。将该帧拼接到原正常输出后，与现代完整输出等长，最大误差不超过 1 LSB。

几个截点下，原 codec 尚未交付帧的 PCM16 峰值如下：

| 已输入编码包数 | 尚未交付帧峰值 |
| ---: | ---: |
| 3 | 8,732 |
| 10 | 15,066 |
| 72 | 12,249 |
| 100 | 19,425 |
| 360 | 15,817 |
| 719 | 0 |

在第 100 个包后调用原 `RAFlush`，其首帧也与上述三种后继得到的 pending PCM 逐字节相同。该调用另外返回的两个帧没有被添加到重建输出或参考 PCM：它们需要额外的核心处理，不能仅根据原 Flush 有返回值便认定它们属于文件的有效采样。

以上后继只用于实验取得原 codec 内部已经完成的输出，不向生产播放过程添加虚构编码包。测试参考由“原正常输出 + 已证明独立于后继内容的一个 pending 帧”组成，而不是复制现代输出建立参考。

## 34 个预设的完整 PCM

原库有效预设为 0–33。每个预设通过 `RAInitEncoder` / `RAEncode` 编码 100 个帧，PCM 输入为确定性的双正弦信号。容器头的采样率、声道数、位深、Cook extra data 来自 `RAGetFlavorProperty`，再封装为合法 RM 音频流。这些属于原编码器生成的有效样本，不冒充从网络取得的历史文件。

| 类别 | 预设 | 覆盖 |
| --- | --- | --- |
| 单声道 | 0–8、14–16 | 8,000 / 11,025 / 22,050 / 44,100 Hz，256 / 512 / 1024 采样帧 |
| 独立双声道 | 9–13 | 两个独立噪声状态，11,025 / 22,050 / 44,100 Hz |
| 联合立体声 | 17–29 | 多种联合立体声起始子带和比特分配，包括 Surround 预设 |
| 5.0 | 30 | 3 个 subpacket，mask `0x37` |
| 5.1 | 31–33 | 4 个 subpacket，mask `0x3F`，含单独的 LFE |

34 个文件的完整输出共 5,745,152 个声道采样值；5,073 个采样值相差 1 LSB，其余完全相同。总体 RMS 误差为 0.0297154 LSB，最大误差为 1 LSB。没有使用增益拟合、统一缩放、时间对齐搜索或静音裁切。

原 `ttp_rm.dll` 能打开并完整解码全部 34 个文件；其正常输出与直接调用原 `cook.dll` 的结果逐字节相同。当前重建版的 PCM、9 个定位点、小缓冲读取、延迟打开和重复 EOF 检查均通过。禁用上一轮 LCG 修正的 DLL 在单声道、独立双声道、联合立体声和 5.1 四个代表样本中均未通过 1 LSB 门限，说明新增样本确实覆盖噪声状态兼容实现。

4 个既有历史文件保留为独立回归。与包括 pending PCM 的原版参考比较后，它们的每声道采样数分别为：Cook20 818,176，Cook32 / Cook64 / Cook96 均 735,232。

## 多声道格式声明

`open_container()` 取得的 `AVCodecParameters.ch_layout` 对 Cook 多声道只含声道数量；`cook_decode_init()` 才从每个 20 字节 subpacket 头的 channel mask 合成真实布局，并写入 `AVCodecContext.ch_layout`。

原重建 Reader 在解码器初始化之前保存 `channel_mask_`，随后没有同步解码器得到的布局，所以 5.0 / 5.1 的输出格式声明为 mask 0。PCM 本身的声道数据已经正确，但宿主无法从格式描述直接得到明确的扬声器位置。

应保证以下两个入口给出同一布局：

1. `Open(flags & 1)` 完成解码器初始化后调用 `Format()`。
2. `Open(0)` 延迟打开后，在读取 PCM 之前调用 `Format()`。

本次采用解码器同步：`prepare()` 成功后校验并保存 Cook 的 native mask；多声道 Cook 的延迟 `Format()` 只用已有类型头初始化解码器，不读取压缩音频。不能只在首次 `Read()` 后才改变此前已返回给宿主的格式。布局继续通过现有的声道数量、有效位和 PCM 格式校验。

`prepare()` 初始化失败后释放未打开或未通过验证的 context，避免下次 `Format()` 因 context 非空而误判为成功。两个独立边界验证分别覆盖未知 Cook 版本导致 `avcodec_open2()` 失败，以及解码器成功初始化但 mask 超出宿主支持范围的后置验证失败。

## 私有测试与验收入口

所有探针、生成音频、原 DLL、PCM、测试可执行文件和 REA JSON 都保存在：

```text
rebuild/tests/plugins_followup_20261029p1/cook/
```

测试代码不进入插件源码包或 Actions。核心文件：

- `packet_probe.cpp`：同一解交错包序列直接比较原 codec 与当前 FFmpeg，记录预热、每包输出和 drain。
- `cutpoints.py` / `cutpoints.json`：10 个截点 × 3 个合法后继。
- `encode_probe.cpp` / `generate_fixtures.py`：原编码器生成与 RM 封装。
- `encoder-parity.json`：34 个预设的全 PCM 误差。
- `reader-matrix.json`：原 Reader 的独立验证。该旧 compare 工具对现代 extensible PCM 的误分类另由新的 parity 工具纠正，不能把其四个 `compressed_reader_requires_decoder` 判定当成播放器失败。
- `parity.cpp`：比较完整原版参考、9 个 seek、最小采样帧读取、延迟打开和重复 EOF，并验证多声道 mask。
- `format_failure.cpp`：对损坏多声道类型头连续调用 3 次 `Format()`、2 次 `Read()`，并验证 eager Open 失败；输出指针和 PCM 长度不能残留。
- `stage/run.cmd`：47 组 XP 可执行验收（34 个预设、4 个历史文件、7 个完整编码包截点、2 个多声道初始化失败边界）。

最终发布 DLL `2026.10.29p1`（SHA-256 `fb6fa95e872f2571b5b9824f8adb7e70ead2fb48262fce2e3db9bc7812decedd`）已在 Windows 11、XP SP3、Win7 SP1 各通过全部 47 组：`COOK_EOS_CASES=47 FAILURES=0`。统一日志位于私有 `plugins_followup_20261029p1/suites/{modern,xp,win7}-cook.log`。新增多声道 mask 断言在旧 DLL 上按预期失败。

## 保留边界

- 未取得额外的互联网历史 mono / 独立 stereo / 5.1 样本；公开样本主机连接超时。已通过原编码器覆盖这些合法分支，但它不能替代未来的更多历史文件覆盖。
- 浮点变换的最后舍入仍允许 1 LSB 差异；没有声称逐位完全一致。
- 维持现代完整 PCM 的结束行为。原 Reader 没有交付的一个正常帧属于有证据的兼容差异，不应靠删除有效尾音掩盖。
- 本轮没有升级 FFmpeg、引入新的系统 API、增加原 RealCodec 运行依赖，或改变已恢复的 Cook LCG 算法。
