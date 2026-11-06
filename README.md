# ttp_rm

2026.11.06p1：[剩余差异补全](docs/RM_REMAINING_DIFFERENCES_20261106P1.md)。恢复 ATRAC3 原别名预设与分通道带宽、Cook 输入滤波及输出时序，增加 SIPR 分包／丢包隐藏接口、RA4 音频边界和有界精确定位缓存，并修复遗漏 YY-Thunks 专用 DLL 入口导致的 XP 动态加载退出异常。当前验证和仍保留的数值／样本限制见报告。

2026.11.05p1：[独立编码与兼容恢复](docs/RM_NATIVE_ENCODERS_20261105P1.md)。增加 ATRAC3、Cook、SIPR 独立源码编码、RM 封装和原宿主 Encoder／Thumbnail ABI；扩展非默认 ATRAC3 预热值，修复 SIPR 5k 启动 AGC 和 16k 基音回退。具体已开放预设、实际验收范围和与原库的差异见文档。

2026.11.05：[ATRAC3／Cook／SIPR 补全与旧系统验收](docs/RM_COMPLETION_20261105.md)。修复 RM 短包误判为正常结束、定位取消后查询格式崩溃，进一步恢复 SIPR 数值路径；旧 `.ra` 尾部索引处理保持兼容。

2026.10.29p1：[低码率 SIPR 精度恢复](docs/SIPR_NUMERICS_20261029P1.md) · [Cook 结束流程与多声道修复](docs/COOK_END_OF_STREAM_20261029P1.md) · [E-AC-3 隔离验证](docs/EAC3_ISOLATION_20261029P1.md)。

Cook／DNET 补噪、SIPR 16k 算法及损坏尾部状态修复（2026-10-29）：[Cook](docs/COOK_NOISE_PARITY_20261029.md) · [DNET](docs/DNET_PARITY_20261029.md) · [SIPR](docs/SIPR_PARITY_20261029.md)。

最新补全与验收：[2026-10-24](docs/COMPLETION_20261024.md)。

定位取消恢复与缓存补充：[2026-10-24](docs/SEEK_RECOVERY_20261024.md)。

历史 RealCodec、AAC／AC3／MPEG 后端对照及残余差异：[原版动态对照](docs/ORIGINAL_DECODER_COMPARISON_20261024.md)。

ATRAC3 预热与极性修复、Cook／DNET／SIPR 扩展对照：[2026-10-25](docs/DECODER_PARITY_FIX_20261025.md)。

TTPlayer 5.7.9 兼容的 x86 RealAudio Reader／Encoder 插件。恢复原 Reader 和元数据 ABI，并通过原宿主编码接口提供 RealAudio 转换，使用固定的 FFmpeg 9.0.2 最小静态后端、AtracDEnc 和独立 Cook／SIPR 编码核心；无需额外安装 VC 运行库或 RealCodec 外部 DLL。

## 构建

```powershell
./build.ps1 -Package -SourcePackage
```

需要 VS 2026 C++ Win32 工具、CMake、Python 3.12+、Git for Windows。源码和 GNU make 构建工具自动从官方来源下载并校验 SHA-256。默认使用本项目的 `build` 缓存，独立构建不依赖播放器、其他插件或私有测试目录。

- `build/Release/ttp_rm.dll`：XP SP3／Win7／Windows 10/11 共用，要求 SSE2。
- `ttp_rm-YYYY.MM.DD.zip`：仅 DLL 与 SHA256SUMS。
- `ttp_rm-YYYY.MM.DD-source.zip`：独立对应源码附件，包含本项目、完整固定 FFmpeg／AtracDEnc 源码、兼容修改、GNU make 源码、许可证和重建入口。
- Action 的 Release a Version 使用北京时间日期与同日 pN 版本；自动构建、校验 XP/Win7 导入并打包，不运行或上传私有测试。

源码附件内 `third_party/ffmpeg` 和 `third_party/atracdenc` 会自动作为源目录使用，可修改后重新构建。VC-LTL／YY-Thunks 仍按固定摘要获取。版权和完整许可证保存在仓库与源码附件，不嵌入 DLL。

[重建依据、差异和验证](docs/IMPLEMENTATION_AND_VALIDATION.md) · [依赖及修改记录](docs/DEPENDENCIES.md)

本项目恢复宿主行为和可维护解码实现；现代解码核心与原外部闭源库不保证 PCM 位精确相等。具体已验证范围和剩余边界见文档。

## 可选格式识别接口

2026-10-20：Reader 工厂增加 Probe，Reader 增加 SourceInfo；旧导出和接口槽位保持不变。声明与辅助实现位于 `include/ttpcomm/audio_format_probe.h` 和 `audio_format_probe_impl.h`，不增加运行时 DLL 依赖。协议原件在相邻 `ttpcomm/sdk/AUDIO_FORMAT_PROBE.md`；新建插件指南在相邻 `rebuild/docs/AUDIO_PLUGIN_DEVELOPMENT_GUIDE.md`。原版播放器继续使用旧接口；错误后缀候选选择和更名提示由支持新协议的重建播放器执行。
