include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TTP_ATRACDENC_COMMIT cb170709f836cb43dc0312455188c791159d81dc)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/atracdenc/src/atrac3denc.cpp")
  set(ttp_atracdenc_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/atracdenc")
else()
FetchContent_Declare(ttp_atracdenc
  URL "https://codeload.github.com/dcherednik/atracdenc/tar.gz/${TTP_ATRACDENC_COMMIT}"
  URL_HASH SHA256=64c8febd3cde0c4225611bd3ed399b5aa7a1c2eced2a13f61403c17e88d7c207
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR ttp-no-upstream-build)
FetchContent_MakeAvailable(ttp_atracdenc)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/atrac3-source-path.txt" "${ttp_atracdenc_SOURCE_DIR}\n")
set(ttp_atrac3_core "${CMAKE_CURRENT_BINARY_DIR}/atrac3-core")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_atrac3.py"
  "${ttp_atracdenc_SOURCE_DIR}" "${ttp_atrac3_core}"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/prepare_atrac3.py")
set(ttp_atrac3_sources
  atrac3denc.cpp transient_detector.cpp transient_spectral_upsampler.cpp env.cpp
  atrac/at3/atrac3.cpp atrac/at3/atrac3_bitstream.cpp atrac/atrac_scale.cpp
  atrac/atrac_psy_common.cpp atrac/atrac_enc_cache.cpp qmf/qmf.cpp
  lib/mdct/mdct.cpp lib/bitstream/bitstream.cpp lib/bs_encode/encode.cpp
  lib/fft/kissfft_impl/kiss_fft.c lib/fft/kissfft_impl/tools/kiss_fftr.c)
list(TRANSFORM ttp_atrac3_sources PREPEND "${ttp_atrac3_core}/")
add_library(atrac3_encoder STATIC ${ttp_atrac3_sources}
  "${CMAKE_CURRENT_LIST_DIR}/../src/atrac3_encoder.cpp")
target_include_directories(atrac3_encoder PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src"
  PRIVATE "${ttp_atrac3_core}" "${ttp_atrac3_core}/lib"
  "${ttp_atrac3_core}/lib/fft/kissfft_impl")
target_compile_definitions(atrac3_encoder PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS NDEBUG)
target_compile_features(atrac3_encoder PRIVATE cxx_std_17)
target_compile_options(atrac3_encoder PRIVATE /EHsc /GR- /utf-8 /fp:precise /arch:SSE2)
