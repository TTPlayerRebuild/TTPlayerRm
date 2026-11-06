cmake_minimum_required(VERSION 3.24)
project(TtpRm LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_rm requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
include(cmake/codec.cmake)
include(cmake/atrac3_encoder.cmake)
include(cmake/version.cmake)
add_library(ttp_rm SHARED src/plugin.cpp src/reader.cpp src/encoder.cpp src/encoder_settings.cpp
  src/encoder_backend.cpp src/cook_encoder.cpp src/sipr_encoder.cpp src/sipr_decoder.cpp src/sipr_packet.cpp
  src/rm_muxer.cpp src/encoder_options.rc src/ttp_rm.def
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_rm ttp_rm_version)
target_include_directories(ttp_rm PUBLIC include PRIVATE src)
target_compile_definitions(ttp_rm PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_rm PRIVATE /W4 /permissive- /EHsc /GR- /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_rm PRIVATE codec atrac3_encoder ole32 uuid shlwapi shell32 user32)
# The manual YY-Thunks OBJ integration requires its DLL entry wrapper. It
# registers/releases XP TLS fallback state before forwarding to the real CRT;
# using the ordinary entry can corrupt shared thunk cleanup in a YY-enabled host.
target_link_options(ttp_rm PRIVATE /ENTRY:DllMainCRTStartupForYY_Thunks
  /alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12)
set_target_properties(ttp_rm PROPERTIES PREFIX "" OUTPUT_NAME "ttp_rm")
add_custom_command(TARGET ttp_rm POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_rm>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_rm>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_rm RUNTIME DESTINATION AddIn COMPONENT Runtime)
