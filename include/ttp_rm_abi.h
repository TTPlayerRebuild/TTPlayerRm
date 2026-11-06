#pragma once
#if defined(_USING_V110_SDK71_)
// SDK 7.1A's IID_PPV_ARGS_Helper names IUnknown before its declaration.
// Make the old SDK header valid with MSVC's /permissive- name lookup.
struct IUnknown;
#endif
#include <windows.h>
#include <mmreg.h>
#include <objidl.h>

// x86 interfaces recovered from ttp_rm.dll and the original player.
// Returned strings/formats use CoTaskMemAlloc; buffer storage belongs to the host.
namespace ttp::rm {
inline constexpr GUID iid_addin{
    0xeec6c534, 0xfeba, 0x421e, {0xaa, 0x5d, 0x10, 0xac, 0x66, 0xf9, 0x87, 0x84}};
inline constexpr GUID iid_reader{
    0x30c7c165, 0xc0a9, 0x4204, {0x99, 0x5d, 0x45, 0x6a, 0x76, 0x49, 0x98, 0xfb}};
inline constexpr GUID iid_metadata{
    0x7ad84e00, 0x5fef, 0x4481, {0xb5, 0x32, 0xfb, 0xbd, 0x67, 0x7e, 0x67, 0xc2}};
inline constexpr GUID iid_thumbnail{
    0xb5e770af, 0xdfb0, 0x43e5, {0x9b, 0x0c, 0x3e, 0xe9, 0x8e, 0x7b, 0x62, 0x48}};
inline constexpr GUID cat_reader{
    0x476d15a5, 0xd863, 0x416a, {0x8a, 0x59, 0xa9, 0xc7, 0xd7, 0x2c, 0xe0, 0x4e}};
inline constexpr GUID cat_encoder{
    0x5954a909, 0x9e02, 0x4650, {0x99, 0x5e, 0xf7, 0x53, 0x5e, 0xe2, 0xbd, 0x8b}};
inline constexpr GUID iid_encoder{
    0x04b9a359, 0x7ff7, 0x4ee9, {0x9c, 0xfb, 0xae, 0xbb, 0x3a, 0x33, 0x1d, 0xc7}};
inline constexpr GUID iid_stream_encoder{
    0xffb27df8, 0x77ef, 0x4ba7, {0x89, 0x45, 0x7e, 0x14, 0x4c, 0x38, 0x1c, 0xbe}};
struct Buffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetLength(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Capacity(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Data(BYTE **, DWORD *) = 0;
};
// Optional rebuilt extension, not an original RA DLL export/vtable. Obtain
// the factory via AddIn::QueryInterface; the original Enum order is unchanged.
inline constexpr GUID iid_sipr_packet_factory{
    0xd7b64785,0xc41b,0x49b3,{0xab,0x0f,0xe7,0x6a,0x3a,0x89,0x72,0x11}};
inline constexpr GUID iid_sipr_packet_decoder{
    0xc01a659c,0x878c,0x4aac,{0x91,0xc3,0x89,0x65,0x9e,0x0c,0x42,0x27}};
struct SiprPacketFormat {
    DWORD size,sample_rate,packet_bytes,packet_samples,core_samples,delay_samples;
};
struct SiprPacketDecoder : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Format(SiprPacketFormat *) = 0;
    // Packed codec bits, at most 32 core frames per call; validity is LSB first
    // per core frame (two per packet at 5/6.5k). Unused trailing bits are ignored.
    // Output PCM16 mono, capacity >= complete_core_frames*core_samples*2.
    // Validation/capacity errors consume nothing. A synthesis or output
    // publication (final SetLength) failure requires
    // Reset; no partial output is published. Calls must be serialized.
    virtual HRESULT STDMETHODCALLTYPE Decode(const BYTE *,DWORD bytes,DWORD validity,Buffer *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
};
struct SiprPacketDecoderFactory : IUnknown {
    // bitrate=5000/6500/8500/16000; flags bit0 retains original 5k delay.
    // No other flags are accepted. The caller releases the returned interface.
    virtual HRESULT STDMETHODCALLTYPE Create(DWORD bitrate,DWORD flags,void **) = 0;
};
struct Reader : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(IStream *, DWORD flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Capabilities(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Duration(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Format(WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferSize(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CodecName(wchar_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Bitrate(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE BitDepth(WORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Read(Buffer *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Seek(DWORD *) = 0;
};
struct Metadata : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Count(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE At(DWORD, wchar_t **, wchar_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Get(const char *, wchar_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Set(const char *, const wchar_t *) = 0;
};
// Optional extension, separate from all recovered vtables. Commit is terminal:
// it closes decoding and performs one save attempt. Repeated calls return the
// same result; Release must never retry an explicit failure. See docs/COMMIT_INTERFACE.md.
inline constexpr GUID iid_metadata_commit{
    0x3ab643c1, 0xd8a4, 0x4d49, {0x9c, 0xbb, 0xef, 0x19, 0x6b, 0x04, 0xb6, 0xe7}};
struct MetadataCommit : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Commit() = 0;
};
#pragma pack(push, 4)
struct Picture {
    DWORD size;
    const wchar_t *mime;
    const wchar_t *description;
    DWORD bytes;
    const BYTE *data;
    DWORD type;
};
#pragma pack(pop)
static_assert(sizeof(Picture) == 24);
struct Thumbnail : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE PictureCount(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MaximumBytes(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE MaximumCount(DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE PictureAt(DWORD, const Picture **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReplacePicture(DWORD, const Picture *, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddPicture(const Picture *) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemovePicture(DWORD) = 0;
};
struct Creator : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Create(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Name(wchar_t **) = 0;
    // ReaderCreator appends slot 5 below.
};
struct ReaderCreator : Creator {
    virtual HRESULT STDMETHODCALLTYPE Extensions(wchar_t **) = 0;
};
// Same x86 slots as the original player's Encoder/EncoderCreator contracts.
struct Encoder : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(const wchar_t *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Finish() = 0;
    virtual HRESULT STDMETHODCALLTYPE Write(Buffer *, DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extension(wchar_t **) = 0;
};
struct StreamEncoder : Encoder {
    virtual HRESULT STDMETHODCALLTYPE OpenStream(IStream *, WAVEFORMATEX *) = 0;
};
struct EncoderCreator : Creator {
    virtual HRESULT STDMETHODCALLTYPE Extension(wchar_t **) = 0;
    virtual HRESULT STDMETHODCALLTYPE Configurable() = 0;
    virtual HRESULT STDMETHODCALLTYPE Configure(HWND) = 0;
};
struct AddIn : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Enum(DWORD, GUID *, void **) = 0;
};
} // namespace ttp::rm
