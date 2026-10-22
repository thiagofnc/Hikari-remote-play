// Workaround for a mingw link error in hikari_host_core (host/src/encoder.cpp):
// it takes `ComPtr<ICodecAPI>` via As(), which needs __uuidof(ICodecAPI), but
// mingw's <strmif.h> declares ICodecAPI without __CRT_UUID_DECL (only
// <icodecapi.h> carries it), leaving __mingw_uuidof<ICodecAPI>() undefined.
// Emitting the (inline, COMDAT) specialization here satisfies that reference
// and merges harmlessly once encoder.cpp includes <icodecapi.h> itself.
// Not a translation unit that includes encoder.h on purpose, so a fix in that
// header cannot collide with this one. Empty under MSVC.

#if defined(__MINGW32__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <icodecapi.h>

namespace hikari::tool {
// Taking the address forces an out-of-line copy (a plain call gets inlined).
extern const GUID& (*const codec_api_uuid_for_mingw)();
const GUID& (*const codec_api_uuid_for_mingw)() = &__mingw_uuidof<ICodecAPI>;
}  // namespace hikari::tool
#endif
