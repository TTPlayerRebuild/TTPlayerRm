# Fixed dependencies and reproducible build

The host wrapper is MIT licensed. The statically linked FFmpeg components and the new Cook/SIPR encoder cores are LGPL 2.1 or later; AtracDEnc and KissFFT retain their licenses below. Complete license texts and copyright credits are retained in `LICENSES/`; no license resource is embedded in the DLL.

| Dependency | Version | Source SHA-256 |
|---|---|---|
| FFmpeg | 9.0.2 | `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e` |
| AtracDEnc (ATRAC3-only core, includes KissFFT) | `cb170709f836cb43dc0312455188c791159d81dc` | `64c8febd3cde0c4225611bd3ed399b5aa7a1c2eced2a13f61403c17e88d7c207` |
| GNU make (build tool only) | 4.4.1 | `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3` |
| VC-LTL | 5.3.1 | `7a18799ed3aa84a225610a5447a56bc534c5c98ccb8dec05caba0e3f633431ad` |
| YY-Thunks | 1.2.2 | `518ed7ef4825e8a41997fbccfa2c8090cf31a6038fd51520a2e49886f947f9fc` |

Sources: [FFmpeg official release archive](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz), [GNU make official source](https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz), [VC-LTL](https://github.com/Chuyu-Team/VC-LTL5/releases/tag/v5.3.1), [YY-Thunks](https://github.com/Chuyu-Team/YY-Thunks/releases/tag/v1.2.2).

Build sources are downloaded and digest-checked by `cmake/build_ffmpeg.py` and CMake FetchContent; they are not vendored. The normal cache is this project's `build/ffmpeg`, not the private test tree. GNU make is bootstrapped from source with MSVC and is never linked into or distributed with the plugin. Git for Windows (Bash), Python 3.12 or newer, CMake 3.24 or newer, and MSVC Win32 build tools are required. Python 3.14 and VS 2026 were used for local verification.

Manual linking of the YY-Thunks OBJ also requires its DLL entry wrapper. The x86 target sets `/ENTRY:DllMainCRTStartupForYY_Thunks` and `/alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12`, as specified by the fixed dependency's Readme. This restores XP dynamically loaded DLL TLS allocation and cleanup before forwarding to the real CRT entry. Merely linking the OBJ can leave the plugin sharing an incorrect TLS index with a YY-enabled host and crash on process exit. The 2026.11.06p1 correction was first verified by relinking unchanged codec objects and repeating the failing load-only probe; it is not a codec algorithm or test-helper workaround.

The FFmpeg profile disables autodetection, network protocols, programs, filters, devices, external libraries, assembly and swscale. The RM profile enables static libswresample for encoder input conversion; other profiles retain their existing setting. GPL, nonfree, and version3 components remain disabled; generated `config.h` records `LGPL version 2.1 or later`. Only the selected container and decoders are enabled. The standalone source, pinned dependency locations and complete build recipes allow rebuilding/relinking against a modified LGPL backend; binary redistributors must retain the applicable license and source/relink provisions.

## Independent ATRAC3 encoder

`cmake/atrac3_encoder.cmake` fetches the pinned [AtracDEnc source](https://github.com/dcherednik/atracdenc/tree/cb170709f836cb43dc0312455188c791159d81dc), checks its archive digest, and builds only its LGPL-2.1 ATRAC3 core and BSD-3-Clause KissFFT. No original RealCodec DLL, Media Foundation, libsndfile or proprietary encoder is linked or loaded. Full license texts and credits are in `LICENSES/AtracDEnc-*` and `LICENSES/KissFFT-BSD-3-Clause.txt`.

`prepare_atrac3.py` derives a separate build tree: it removes unused ATRAC1/ATRAC3+ template instances, makes the shared ATH table immutable to avoid concurrent-constructor races, and converts internal fatal aborts into exceptions caught by the plugin wrapper. It checks exact pinned patch context instead of silently applying a partial patch. The wrapper adds the required initial padding to match RM's 2190-sample delay, preserves the caller's floating-point rounding mode, bounds buffering, and flushes the final partial frame. The 2026.11.06p1 update also bounds ordinary and tonal allocation independently per coded channel using the original bitrate-dependent bandwidths. Dynamic comparisons establish that the five original Surround flavors are aliases of five base flavors; they are now restored. This does not claim bit-identical psychoacoustic decisions.

The corresponding-source ZIP contains the complete fixed AtracDEnc tree in `third_party/atracdenc`, not just the copied build subset. CMake automatically selects that bundled source; users can modify it and rebuild the statically linked LGPL core. The runtime ZIP remains DLL + SHA256SUMS only.

`src/cook_encoder.cpp` and `src/sipr_encoder.cpp` are LGPL-2.1-or-later independent encoder implementations. They use the pinned FFmpeg transform/codebook definitions and, for SIPR 16k analysis-by-synthesis, its statically linked reconstruction function. The SIPR encoder explicitly uses the RM backend's private `SiprContext` layout and checks the size/offsets at compilation. No source or library from the AGPL NihAV implementation is included. Original closed DLLs are private test references only.

## Local FFmpeg modification

`libavcodec/twinvq.c`: when the build defines `TTP_TWINVQ_KEEP_LEADING_FRAMES`, expose the first two synthesis frames instead of discarding them in FFmpeg, and zero the initial current/previous overlap histories. The zeroing is required because upstream never exposes those unprimed frames; without it, the legacy direct-Read path can emit stale heap contents. This macro is enabled only for the VQF profile. Original TTPlayer performs the discard in Reader.SetCallback (`801D1AD7`), rather than in the low-level decoder. The wrapper restores that sequencing. Other codec profiles have unchanged TwinVQ behavior and do not compile this decoder.

The configure output and build identity record are preserved in the build directory. Script/configuration changes force a clean backend rebuild. Windows native GNU make requires normalized drive-letter paths and disabled localized MSVC include-dependency scanning; these changes affect build files, not codec algorithms.


RM profile additionally patches `libavformat/rmdec.c` behind `TTP_RM_MP3_ADU` to recognize the original `audio/X-MP3-draft` MIME before the zero-length codec-header return. It restores MP3 ADU/audio classification and millisecond timestamps. This patch is inactive in every other profile. Enabled RM decoders are cook, atrac3, sipr, ra_144, ra_288, aac, ac3, eac3 (shared MSVC declarations) and mp3adu.

The 2026-10-29 RM compatibility profile also applies three checked, idempotent modifications:

- `TTP_RM_LEGACY_COOK`: `legacy_cook.py` restores the original per-core LCG and zero-coefficient sign selection in `cook.c`.
- `TTP_RM_LEGACY_DNET`: `legacy_dnet.py` restores the original AC-3 LFSR and independent coupled-channel noise in `ac3dec.c` / `ac3dec.h`; E-AC-3 keeps the upstream branch.
- `TTP_RM_LEGACY_SIPR`: `legacy_sipr.py` and the two `sipr_legacy16k*.h` files restore the original 16 kbps LSF/ISP/LPC transform in `sipr16k.c`. The read-only quantizer tables record their binary provenance; lower-rate synthesis is unchanged.

The 2026.10.29p1 update extends the same SIPR switch to `sipr.c` via
`sipr_legacylow.h` and `sipr_legacylow_tables.h`: original Hz codebooks,
ISP/LPC arithmetic, the 5 kbps primed initial ISP, and an SIPR-only 20-tap
adaptive-pitch interpolation table and accumulation order. Shared
`ff_b60_sinc` and other codecs are unchanged. All new headers are tracked
by CMake and the complete source hash; no new dependency is introduced.

All three are enabled only in the RM profile. CMake tracks the patch modules and headers; the complete patched source digest participates in the backend build identity. No old RealCodec binary is required. See the dated Cook, DNET and SIPR documents for original-function evidence, residual differences and validation.

The 2026.11.05 update restores the original low-rate and 16 kbps SIPR gain
float stores and the low-rate four-state high-pass filter. With
`TTP_RM_LEGACY_SIPR`, the private `SiprContext::highpass_filt_mem` has four
floats; with the macro disabled it retains the upstream two-float layout.
All backend objects must be rebuilt together. The build identity and CMake
dependencies cover these changes; no Reader ABI or external dependency changes.

`legacy_rm_packets.py`, enabled only by the RM profile's
`TTP_RM_STRICT_PACKETS`, checks physical short reads in the INT4, GENR and
SIPR deinterleavers of `libavformat/rmdec.c`. Incomplete RM audio packets
return invalid-data instead of letting zero-filled bytes reach the decoder.
The raw `.ra` path has no RM packet envelope. The 2026.11.06p1
`legacy_raw_ra.py` patch bounds known-length RA4 audio using its separate
audio-size and chunk-size fields. `RaTC` is a FourCC/offset chunk directory,
not a time index. Optional directory corruption or ordinary optional I/O
failure does not truncate declared audio; restoring the actual audio stream
position is mandatory, and cancellation remains an error. Known-length
physical truncation and incomplete interleave groups are rejected. Unknown
lengths and other old header variants retain their previous path. The patches
are idempotent; this is not a claim that every raw-RA truncation is covered.

`-SourcePackage` adds a separate corresponding-source ZIP: complete FFmpeg source including compatibility changes, wrapper source, official GNU make source archive, full licenses and scripts. It excludes private tests, samples, build products and caches. The source bundle automatically selects `third_party/ffmpeg`; its complete source digest participates in the build identity so modifications trigger recompilation. Runtime ZIP contents remain DLL + SHA256SUMS only.

`legacy_atrac.py`, behind RM-only `TTP_RM_LEGACY_ATRAC`, accepts the original meaningful ATRAC3 priming range `0..32767` in the 10/12-byte RM headers instead of accepting only 2190. Other header checks are preserved. The original sign-extends this field; values with the high bit set lead to an unusable huge discard count and remain rejected rather than reproducing that behavior.

The encoder update also extends `legacy_sipr.py` under the existing RM-only switch. In 5k mode, initialization retains the original hidden startup frame's AGC/ISP/history state without reintroducing its 240-sample latency in the ordinary Reader. The 2026.11.06p1 version synthesizes that hidden frame instead of retaining only the former AGC constant `0x3f1119fe`. In 16k mode, the second-subframe pitch 62/63 fallback uses the original previous pitch + 1 and zero fractional phase. These bit patterns are not rejected as invalid. All ordinary 0..61 paths remain unchanged; the native encoder searches that ordinary range. The patches upgrade an already-patched source tree idempotently and leave builds without the compatibility macro unchanged.

The 2026.11.06p1 RM profile adds `sipr_legacyplc.h` and a typed private context extension for four-mode erasure concealment, ordered scalar synthesis and AGC arithmetic. The public optional packet API is implemented in `src/sipr_decoder.cpp` and `src/sipr_packet.cpp`; ordinary Reader packets do not enable PLC. Excessive 16k erasure pitch is rejected before an out-of-bounds history read. The source-level SIPR context contract is checked by the encoder and decoder build; all backend objects must be rebuilt together.

`legacy_checkpoints.py` installs typed Cook, ATRAC3, SIPR and RM-demux state helpers. The helpers validate codec configuration, extradata, sizes and numeric state; they do not serialize pointers or copy complete private codec objects. Generic packet queues must be empty, and only eligible complete GENR/INT4/SIPR matrices are cached. Int0/multiframe packets keep linear replay. Reader checkpoints are limited to 8 MiB/256 entries per instance, separate from the existing 4 MiB PCM history. They are process-local and never persisted. All helper headers and patch scripts participate in CMake dependencies and the complete source digest.

Cook encoding now applies the original three-biquad input filter before attack analysis and MDCT. Cook Reader publication holds one decoded block until the next successful block, matching the original host's non-flushing EOF path. The encoder's required tail packets remain intact. See [the 2026.11.06p1 report](RM_REMAINING_DIFFERENCES_20261106P1.md) for evidence and remaining numerical differences.
