# RealAudio 重建：依据、行为和验证

## 1. 原版版本绑定

对照 DLL 是 `AddIn/ttp_rm.dll`（62,304 字节），SHA-256：
`3b2a0f2a44f795e7c5984fdfaa9875af0cb6f1721fbd9370321714162a2faf5b`。

伪代码为同一二进制的 Ghidra 12.1.3 导出。以下地址是原 DLL 虚拟地址，不是新版符号。

| 原地址 | 行为 | 重建位置 |
|---|---|---|
| `60608EDA / 60608F80 / 60608FCC` | ordinal 1 导出、单 Reader 工厂 | `src/plugin.cpp` |
| `606082CB` | IStream、flags、格式及宿主解码交互 | `src/reader.cpp::Open` |
| `60601652` | RealCodec 外部解码 DLL 装载 | 最小静态 FFmpeg 替代 |
| `6060562C / 606058DD` | `audio/X-MP3-draft` 的 MP3 ADU 支路 | FFmpeg RM MDPR 补丁＋首包格式解析 |
| `60608556 / 606086EA` | PCM／AAC／DNET／ADU 分包 | AVIO、FFmpeg demux／decode、PCM 缓冲 |
| `60607EDE / 60608105 / 606089E5` | 标签枚举、获取、修改／删除 | `src/native_metadata.h` |
| `60605D83 / 60606B60` | RMMD＋RJMD 树＋RMJE＋ID3v1 保存 | 同一文件事务 |
| `606080B9 / 606081F3` | 单封面、最大 60,000 字节 | Thumbnail 原限制 |

## 2. 恢复与替换

保留原工厂名、扩展名串、COM GUID、虚表顺序、CoTaskMemAlloc 返回值以及 Reader／Metadata／Thumbnail 三类接口。增加兼容现有重建宿主的可选 MetadataCommit 接口，原播放器最后一次 Release 的保存方式仍可使用。

原插件对 Cook／ATRC／SIPR 调用 RealCodec，对部分 AAC／AC3／MP3 ADU 输出压缩帧交给宿主。新版在 DLL 内完成解码并统一返回 PCM16，避免原宿主外部解码器与运行库的组合缺失。这是明确的后端替换；不会宣称旧压缩帧 ABI 的逐字节输出也相同。

启用的 FFmpeg 音频解码器为 Cook、ATRAC3、SIPR、RA144、RA288、AAC、AC3、MP3 ADU。EAC3 与 AC3 共用 MSVC 声明，作为编译依赖启用；不包含视频解码器、网络协议或其他外部编解码库。GPL／nonfree／version3 配置均检查为 0。

`audio/X-MP3-draft-00` 是实际发现的原支持分支。FFmpeg 9.0.2 未识别该 MIME，默认将它视作 data 流。构建补丁只在 RM profile 定义 `TTP_RM_MP3_ADU` 时生效：恢复 audio 类型、MP3ADU codec、毫秒 time base。首包的 MPEG 头给出采样率／声道；该包保留用于后续解码，不丢首包。

## 3. I/O、定位和生命周期

- flags=0 仅打开容器及信息；第一次 Read／Seek 按需准备解码器。
- 使用原宿主扩展 ReadWait 和回调接口；普通 IStream 可用。
- 保护容量、采样对齐、零容量、短读、解码错误和 EOF 长度归零。
- 自定义 AVIO 的绝对位置以传入 IStream 的初始位置为基准；前置 137 字节测试不影响 PCM。
- Stop 使用原子标志及事件，Start 后可继续读取；不依赖进程全局解码状态。
- 先识别 RMMD／RJMD／ID3 尾部，并以原媒体结尾限制 AVIO Read、Size 与 SEEK_END。旧原始 `.ra` 没有 DATA 对象长度，若直接暴露整个文件，保存后的标签会被当成压缩音频，造成多余声音及错误时长；RA28.8 保存后 PCM 和时长均已验证与保存前一致。
- 定位通过重新解码／丢弃达到样本位置；向后重开解码器，结果与同一 DLL 完整解码切片一致。尚未实现原索引表跳转的速度优化，长曲目向后定位可能较慢。

## 4. 标签和封面事务

读取有界 RMMD/RJMD 树，保留未知节点的 type、flags、attribute、值和子树。限制总元数据大小、递归深度、节点数、偏移、重叠和长度。损坏元数据不阻止音频播放，但关闭写权限。

标签使用原 Album／Artist／Track 分组；修改文字以系统 ANSI 代码页编码，无法表示的文字报错，不静默变成问号；系统 UTF-8 代码页也有对应 API 参数。读取有效 UTF-8，其余按 ANSI。修改前复制工作状态，错误参数不会留下半完成的脏状态。

封面恢复 Cover Art 二进制节点及 DataSize／MimeType／Description／Extension。支持原单图、60,000 字节限制；PNG 也是可保存的 MIME。ReplacePicture 按位掩码更新，删除保留其他未知元数据。

首次编辑将旧 CONT 字段移入 RJMD。同一事务中清空 CONT 的文字载荷但不改变对象长度与音频／索引偏移，否则原播放器在删除 Title 后会再次从 CONT 读回旧标题。保存同时维护 ID3v1 的有界兼容镜像。

先保存原字节及生成新暂存文件，再一次性写回、SetSize、Commit；真实写入／flush 错误触发尽力回滚并返回原错误。只容许旧流的 E_NOTIMPL／STG_E_INVALIDFUNCTION Commit 返回；其他失败不伪装成功。检查大小变化和只读属性。可选 Commit 是终止式且记忆结果，Release 不会偷偷重试。

这不是操作系统级原子替换：不可恢复的设备故障可能令回滚本身失败。旧 Release 接口无法把失败返回给原播放器，显式 Commit 可以。

## 5. 本地对照

私有测试位于播放器仓库 `rebuild/tests/audio_plugins_fix_20261019/rm_vqf`，不进入此仓库、Action 或发行包。

- Cook、ATRC、SIPR、RA28.8、AAC、DNET 六类文件完成整曲解码。与本机 FFmpeg 9.0.1 x64 CLI 比较，输出样本数全部相同；RA28.8 整体位相同，其余最大仅一个 PCM16 整数单位差异。
- MP3 ADU 公开样本原 flags=0 与新 flags=0 的时长均 128,705 ms；新版输出 23,657,472 字节，正常 EOF。旧插件单独宿主的 raw Read 返回 E_FAIL，尚不把该分支记作原 PCM 对照成功。
- RA14.4 公开样本本身被截断：末帧仅 17 字节，解码后明确返回错误；不以正常 EOF 掩盖。
- Cook／ATRC／SIPR 的原完整播放对照被原 RealCodec 所需 MSVCR71 缺失阻断；本机及已检查 VM 没有该运行库。不下载来源不明的 DLL。原 flags=0、格式、标签、封面和保存后读回已经执行。
- 原插件能够读回新版保存的标题、单 PNG 封面、描述；已有原 RMMD 文件更新也能由原 DLL 读回。删除 Title 后原 DLL 不再显示旧 CONT 标题。
- 未知二进制节点保存后逐字段相同；恶意越界 RMMD 不影响正常音频解码且不允许编辑。
- 容量／对齐、Stop/Start、500ms 样本定位、双 Reader、31 字节短读、lazy、非零基址已执行。
- E_NOTIMPL／STG_E_INVALIDFUNCTION／短写／flush／外部长度变化／只读／重复 Commit 已执行。短写与 flush 注入后文件 SHA 与操作前一致。

## 6. 未覆盖和差异

原 RealCodec 全部 PCM 位精确对照、更多声道／历史交织 flavor、多码率 SureStream、真实网络慢流、并发 Stop 正好阻塞在第三方 IStream 的时序、GUI 全部控制与错误流程仍需要进一步样本与实测。未实现 RALF／DRM／视频播放；这些不被本构建 profile 宣称支持。`.ram` 文本播放列表应由宿主展开，插件仅处理真实音频容器。

ID3v1 镜像当前将 Genre 编号写为 255，完整流派文字保留在 RJMD；不宣称恢复旧流派编号表。无法定位的原始 `.ra` 流不能预先检查末尾标签，该极端输入仍需专项验证。

## 7. 2026-10-19 跨系统验证

- RM/VQF 联合可移植批次共 50 项，Windows 11 本机、XP SP3、Windows 7 均 `failures=0`。XP／Win7 的 41 个音频和保存后文件 SHA-256 与 Windows 11 基准逐个一致；其中原始 RA28.8 保存后的时长仍为 61,934ms，PCM 与保存前完全相同。
- XP／Win7 上用原 `ttp_rm.dll` 读回新版 PNG 封面、删除后的 Title、原始 `.ra` 的 `Committed title` 均正确。XP 上仅给原 DLL 私有探针目录补入已存在的 MSVCR110，以加载旧二进制；新版 DLL 没有该运行库依赖。原 RealCodec 解码后端所需 MSVCR71 仍缺失，不能因此宣称完成原后端完整 PCM 对照。
- Windows 11、XP SP3、Windows 7 上，原版 5.7.9 播放器及重建版播放器均已实际使用新版 DLL 播放 Cook 样本。模块路径均确认指向新 DLL，PlayingTime 持续推进，无错误弹窗并正常关闭。对应联合宿主记录为私有 `hosts/{win11,xp,win7}-rm-vqf.log`。
- 新 DLL：935,936 字节，SHA-256 `f5690cbc7e9124e89dbbbbcea94ad887ce39f02d957b7fc8bd2e40c84255a2dc`；静态导入检查通过。
- Windows 10 没有独立实机；按用户要求由 Windows 11 承担新系统验证，记录保留实际测试系统。
