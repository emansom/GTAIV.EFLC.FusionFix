// ===========================================================================
// d3d9cache.h — reading D3D9 cache files safely, and sharing them between PCs.
//
// Three jobs, shared by the capture (shadercapture.ixx) and the replay
// (shaderprecompile.ixx):
//
//   * READ a cache file without trusting it. Drop-ins come from other PCs, other
//     FusionFix builds and other mod sets, and can arrive truncated or damaged in
//     transit. Every byte is checked here, from a bounded in-memory copy, before
//     anything reaches D3D9, DXVK or the driver: section bounds and counts, every
//     enum a replay passes to D3D9, every vertex declaration, and every shader's
//     bytecode (its hash, and a token walk that keeps DXVK's reader inside the
//     buffer). Damage to the container rejects the file; a bad key, declaration or
//     shader drops only that item, and the rest of the file is still used.
//
//   * FIND the drop-ins: plugins\d3d9cache\, top level, any .bin name. Nothing in
//     that folder is ever written, moved or renamed, except this PC's own snapshot.
//
//   * SNAPSHOT this PC's own capture(s) into plugins\d3d9cache\ as
//     FusionFix.<h>.bin once per launch, <h> being the FNV-1a of the file's bytes,
//     so the folder is what a player copies between PCs and nobody renames
//     anything. Same content, same name: every loader de-duplicates by content hash.
// ===========================================================================
#pragma once

#include "pipelinekeys.h"
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace d3d9cache
{
    using pipelinekeys::KeyRecord;
    using pipelinekeys::CacheContents;

    // Limits. Real captures are ~5 MB (14,500 keys, 29 declarations, 25 shaders), so
    // these are generous, and they bound every allocation a file can cause.
    constexpr size_t   kMaxFileBytes    = 16u << 20;   // ~50,000 keys
    constexpr uint32_t kMaxSections     = 32;
    constexpr uint32_t kMaxMetaStrings  = 16;
    constexpr uint32_t kMaxStringBytes  = 1u << 16;
    constexpr uint32_t kMaxShaderBytes  = 1u << 20;
    constexpr uint32_t kMaxDeclElements = MAXD3DDECLLENGTH + 1;   // incl. D3DDECL_END
    constexpr uint32_t kOldestVersion   = 1;

    // Container v1 is v2 without KeyRecord::streamFreq; everything else is the same.
    constexpr size_t kRecordV1 = offsetof(KeyRecord, streamFreq) + 2 * sizeof(uint32_t);

    // ---- names ---------------------------------------------------------------
    // The content hash that names and de-duplicates files: 64-bit FNV-1a with the
    // standard offset basis, over the whole file. NOT pipelinekeys::Fnv1a, whose
    // basis is 1469598103934665603 -- the standard one with its last digit lost --
    // and which cannot be fixed, because every shader and key hash in every cache
    // file is computed with it. The Vulkan side names its files with this one too.
    inline uint64_t ContentHash(const void* data, size_t len)
    {
        auto p = static_cast<const uint8_t*>(data);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
        return h;
    }

    // FusionFix.<16 lowercase hex digits of ContentHash>.bin
    inline std::string ContentName(uint64_t h)
    {
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "FusionFix.%016llx.bin", (unsigned long long)h);
        return buf;
    }

    // Only names of exactly that shape: the snapshot deletes nothing else.
    inline bool IsContentName(const std::string& n)
    {
        if (n.size() != 10 + 16 + 4 || n.compare(0, 10, "FusionFix.") != 0 || n.compare(26, 4, ".bin") != 0)
            return false;
        for (size_t i = 10; i < 26; i++)
            if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return false;
        return true;
    }

    inline bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
    {
        const size_t n = wcslen(suffix);
        return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
    }

    inline std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    inline std::wstring Wide(const std::string& a)
    {
        if (a.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), nullptr, 0);
        std::wstring w(n > 0 ? n : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_ACP, 0, a.c_str(), (int)a.size(), &w[0], n);
        return w;
    }

    // A metadata string from a file, fit for one log line: at most 80 characters and
    // no control characters, so a file cannot break or forge lines of the log.
    inline std::string Printable(const std::string& s)
    {
        if (s.empty()) return "?";
        std::string o = s.substr(0, 80);
        for (auto& ch : o)
            if ((unsigned char)ch < 0x20 || ch == 0x7F) ch = '?';
        return o;
    }

    // plugins\, beside the ASI, with a trailing backslash. Empty if unknown.
    inline std::wstring PluginsDir()
    {
        wchar_t buf[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PluginsDir), &self);
        DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (!n || n >= MAX_PATH) return std::wstring();
        std::wstring s(buf);
        auto slash = s.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
    }

    inline std::wstring DropInDir()
    {
        std::wstring p = PluginsDir();
        return p.empty() ? p : p + L"d3d9cache\\";
    }

    // ---- reading -------------------------------------------------------------
    // The whole file, or false with a reason. Never more than `cap` bytes.
    inline bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, size_t cap,
                              std::string& why, bool& absent)
    {
        absent = false;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            const DWORD e = GetLastError();
            absent = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);
            why = "cannot be opened (error " + std::to_string(e) + ")";
            return false;
        }
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) != 0;
        if (!ok) why = "size unknown";
        else if (size.QuadPart == 0) { why = "empty (0 bytes)"; ok = false; }
        else if ((unsigned long long)size.QuadPart > cap)
        {
            why = "too large (" + std::to_string((unsigned long long)size.QuadPart) + " bytes, limit " +
                  std::to_string((unsigned long long)cap) + ")";
            ok = false;
        }
        if (ok)
        {
            out.resize((size_t)size.QuadPart);
            DWORD got = 0;
            if (!ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) || got != out.size())
            {
                why = "read failed";
                ok = false;
            }
        }
        CloseHandle(h);
        if (!ok) out.clear();
        return ok;
    }

    // ---- validation: every value that reaches a D3D9 call ---------------------
    inline bool IsFourCC(uint32_t f, const char (&s)[5])
    {
        return f == ((uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
                     ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24));
    }

    // Colour formats a render target can have (d3d9types.h), plus the NULL FOURCC.
    // Whether THIS device can render to one is asked at replay; this only keeps
    // values D3D9 never defined away from it.
    inline bool IsColorFormat(uint32_t f)
    {
        return (f >= 20 && f <= 36) || f == 40 || f == 41 || (f >= 50 && f <= 52) || (f >= 60 && f <= 64) ||
               f == 67 || f == 81 || (f >= 110 && f <= 119) || IsFourCC(f, "NULL");
    }

    inline bool IsFourCCDepth(uint32_t f)
    {
        return IsFourCC(f, "INTZ") || IsFourCC(f, "DF24") || IsFourCC(f, "DF16") || IsFourCC(f, "RAWZ");
    }

    inline bool IsDepthFormat(uint32_t f)
    {
        return f == 70 || f == 71 || f == 73 || f == 75 || f == 77 || f == 79 || f == 80 ||
               (f >= 82 && f <= 85) || IsFourCCDepth(f);
    }

    // nullptr if `v` is a value D3D9 defines for render state `rs`, else what it is
    // not. Only the enum- and float-valued states are checked: booleans, masks and
    // reference values take any DWORD in D3D9. Keyed by state, not by position, so
    // it cannot drift from kTrackedRS.
    inline const char* CheckRS(D3DRENDERSTATETYPE rs, uint32_t v)
    {
        switch (rs)
        {
        case D3DRS_ZENABLE:
            return v <= D3DZB_USEW ? nullptr : "D3DZBUFFERTYPE";
        case D3DRS_ZFUNC: case D3DRS_ALPHAFUNC: case D3DRS_STENCILFUNC: case D3DRS_CCW_STENCILFUNC:
            return (v >= D3DCMP_NEVER && v <= D3DCMP_ALWAYS) ? nullptr : "D3DCMPFUNC";
        case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA:
            return (v >= D3DBLEND_ZERO && v <= D3DBLEND_INVSRCCOLOR2) ? nullptr : "D3DBLEND";
        case D3DRS_BLENDOP: case D3DRS_BLENDOPALPHA:
            return (v >= D3DBLENDOP_ADD && v <= D3DBLENDOP_MAX) ? nullptr : "D3DBLENDOP";
        case D3DRS_CULLMODE:
            return (v >= D3DCULL_NONE && v <= D3DCULL_CCW) ? nullptr : "D3DCULL";
        case D3DRS_FILLMODE:
            return (v >= D3DFILL_POINT && v <= D3DFILL_SOLID) ? nullptr : "D3DFILLMODE";
        case D3DRS_SHADEMODE:
            return (v >= D3DSHADE_FLAT && v <= D3DSHADE_PHONG) ? nullptr : "D3DSHADEMODE";
        case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS:
        case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS:
            return (v >= D3DSTENCILOP_KEEP && v <= D3DSTENCILOP_DECR) ? nullptr : "D3DSTENCILOP";
        case D3DRS_FOGTABLEMODE: case D3DRS_FOGVERTEXMODE:
            return v <= D3DFOG_LINEAR ? nullptr : "D3DFOGMODE";
        case D3DRS_DIFFUSEMATERIALSOURCE: case D3DRS_SPECULARMATERIALSOURCE:
        case D3DRS_AMBIENTMATERIALSOURCE: case D3DRS_EMISSIVEMATERIALSOURCE:
            return v <= D3DMCS_COLOR2 ? nullptr : "D3DMATERIALCOLORSOURCE";
        case D3DRS_VERTEXBLEND:
            return (v <= D3DVBF_3WEIGHTS || v == D3DVBF_TWEENING || v == D3DVBF_0WEIGHTS) ? nullptr : "D3DVERTEXBLENDFLAGS";
        case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
        {
            float f;
            memcpy(&f, &v, 4);
            return std::isfinite(f) ? nullptr : "finite float";
        }
        default:
            return nullptr;
        }
    }

    // A flexible vertex format built only from bits D3D9 defines.
    inline bool CheckFVF(uint32_t fvf)
    {
        const uint32_t known = 0x400Eu | 0x0010u | 0x0020u | 0x0040u | 0x0080u | 0x0F00u | 0x1000u | 0x8000u | 0xFFFF0000u;
        if (fvf & ~known) return false;
        const uint32_t pos = fvf & 0x400Eu;
        if (pos != 0 && pos != 0x4002u && (pos & 0x4000u)) return false;
        return ((fvf >> 8) & 0xFu) <= 8;
    }

    // A vertex declaration D3D9 would accept and the replay can bind: known types,
    // methods and usages, DWORD-aligned offsets, only the streams the replay binds
    // (and the capture records instancing for), and D3DDECL_END last and only last.
    // DXVK walks the array to D3DDECL_END, so a missing one would read past it.
    inline const char* CheckDecl(const D3DVERTEXELEMENT9* e, uint32_t n)
    {
        if (n == 0 || n > kMaxDeclElements) return "element count out of range";
        for (uint32_t i = 0; i < n; i++)
        {
            if (e[i].Stream == 0xFF)
            {
                if (i != n - 1) return "D3DDECL_END before the last element";
                if (e[i].Offset != 0 || e[i].Type != D3DDECLTYPE_UNUSED || e[i].Method != 0 ||
                    e[i].Usage != 0 || e[i].UsageIndex != 0)
                    return "malformed D3DDECL_END";
                return nullptr;
            }
            if (e[i].Stream >= pipelinekeys::kMaxStreams) return "stream index out of range";
            if (e[i].Offset >= 512 || (e[i].Offset & 3)) return "offset out of range";
            if (e[i].Type > D3DDECLTYPE_FLOAT16_4) return "not a D3DDECLTYPE";
            if (e[i].Method > D3DDECLMETHOD_LOOKUPPRESAMPLED) return "not a D3DDECLMETHOD";
            if (e[i].Usage > D3DDECLUSAGE_SAMPLE) return "not a D3DDECLUSAGE";
            if (e[i].UsageIndex >= 16) return "usage index out of range";
        }
        return "no D3DDECL_END";
    }

    // One recorded key. `why` names the first bad field.
    inline bool CheckKey(const KeyRecord& k, std::string& why)
    {
        using namespace pipelinekeys;
        char buf[160];
        auto fail = [&](const char* fmt, auto... a) {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, a...);
            why = buf;
            return false;
        };
        if (k.primType < D3DPT_POINTLIST || k.primType > D3DPT_TRIANGLEFAN)
            return fail("primitive type %u is not a D3DPRIMITIVETYPE", k.primType);
        if (k.declIndex == kDeclNone && !CheckFVF(k.fvf))
            return fail("FVF 0x%x has undefined bits", k.fvf);
        for (uint32_t i = 0; i < kMaxRT; i++)
            if (k.rtFmt[i] && !IsColorFormat(k.rtFmt[i]))
                return fail("render target %u format %u is not a colour D3DFORMAT", i, k.rtFmt[i]);
        if (k.dsFmt && !IsDepthFormat(k.dsFmt))
            return fail("depth format 0x%x is not a depth D3DFORMAT", k.dsFmt);
        if (k.msType > D3DMULTISAMPLE_16_SAMPLES || k.msQuality > 255 || (!k.msType && k.msQuality))
            return fail("multisample %u/%u out of range", k.msType, k.msQuality);
        for (uint32_t i = 0; i < kNumRS; i++)
            if (const char* t = CheckRS(kTrackedRS[i].rs, k.rs[i]))
                return fail("%s = %u is not a %s", kTrackedRS[i].name, k.rs[i], t);
        for (uint32_t i = 0; i < kNumSamplers; i++)
            if (k.samplerType[i] > kSamplerVolume)
                return fail("sampler %u type %u out of range", i, (uint32_t)k.samplerType[i]);
        // Exactly what InstanceFreq() records: 0, or INSTANCEDATA | a divisor >= 1.
        // Stream 0 is never instance data (DXVK rejects it; the replay draws with
        // INDEXEDDATA there).
        for (uint32_t s = 0; s < kMaxStreams; s++)
        {
            const uint32_t f = k.streamFreq[s];
            if (!f) continue;
            if (s == 0 || !(f & D3DSTREAMSOURCE_INSTANCEDATA) || (f & 0x7F800000u) || !(f & 0x7FFFFFu))
                return fail("stream %u frequency 0x%x is not instance data", s, f);
        }
        return true;
    }

    // SM2/SM3 register limits per register type (D3DSHADER_PARAM_REGISTER_TYPE),
    // index must be below; 0 = the type does not exist in that stage. These are the
    // D3D9 limits, which are within every fixed-size table DXVK indexes by them.
    inline uint32_t RegisterLimit(bool vs, uint32_t type)
    {
        static const uint16_t kVS[20] = { 32, 16, 256, 1, 3, 2, 12, 16, 0, 0, 4, 0, 0, 0, 16, 1, 0, 0, 2048, 1 };
        static const uint16_t kPS[20] = { 32, 10, 224, 8, 0, 0, 0, 16, 4, 1, 16, 0, 0, 0, 16, 1, 0, 2, 2048, 1 };
        return type < 20 ? (vs ? kVS[type] : kPS[type]) : 0;
    }

    inline bool RegisterOk(bool vs, uint32_t tok)
    {
        if (!(tok & 0x80000000u)) return false;                        // not a parameter token
        const uint32_t type = ((tok >> 28) & 0x7u) | ((tok >> 8) & 0x18u);
        return (tok & 0x7FFu) < RegisterLimit(vs, type);
    }

    // Shader bytecode as D3D9 hands it to the driver: a version token, instructions
    // and a D3DSIO_END. CreateVertexShader/CreatePixelShader take no length, and
    // DXVK reads until it meets END, so a stream without one inside the buffer would
    // be read past its end. Walked with DXVK's own rules (SM2+ encodes each
    // instruction's length), and every register an instruction names is checked
    // against the D3D9 limits, which also bounds the tables DXVK indexes with them.
    // Only SM2/SM3: GTA IV and FusionFix use SM3, and SM1 has no length field.
    inline const char* CheckBytecode(uint32_t stage, const uint8_t* code, uint32_t size)
    {
        if (size < 8 || (size & 3)) return "size is not whole tokens";
        const uint32_t n = size / 4;
        auto tok = [&](uint32_t i) { uint32_t v; memcpy(&v, code + 4 * i, 4); return v; };

        const uint32_t ver = tok(0);
        const bool vs = stage == pipelinekeys::kStageVS;
        if ((ver >> 16) != (vs ? 0xFFFEu : 0xFFFFu)) return "version token is not this stage";
        const uint32_t major = (ver >> 8) & 0xFF, minor = ver & 0xFF;
        if (major < 2 || major > 3 || (major == 3 && minor != 0)) return "not shader model 2 or 3";

        uint32_t inputDcls = 0;
        for (uint32_t i = 1; i < n;)
        {
            const uint32_t t = tok(i);
            const uint32_t op = t & 0xFFFF;
            if (t == 0x0000FFFFu) return nullptr;                      // D3DSIO_END
            if (op == 0xFFFE)                                          // comment
            {
                const uint32_t len = (t >> 16) & 0x7FFF;
                if ((t & 0x80000000u) || len > n - i - 1) return "comment runs past the end";
                i += 1 + len;
                continue;
            }
            if (t & 0x80000000u) return "parameter token where an instruction belongs";
            // The SM2/SM3 instruction set: nop..defi, texkill, texld, expp, logp, def,
            // cmp, dp2add, dsx, dsy, texldd, setp, texldl, breakp. The rest are SM1.
            const bool known = op <= 48 || op == 65 || op == 66 || op == 78 || op == 79 || op == 81 ||
                               op == 88 || (op >= 90 && op <= 96);
            if (!known) return "not an SM2/SM3 instruction";
            const uint32_t len = (t >> 24) & 0xF;
            if (len > n - i - 1) return "instruction runs past the end";

            uint32_t first = i + 1, regs = len;
            if (op == 31)                                              // dcl: usage token + register
            {
                if (len != 2) return "malformed dcl";
                const uint32_t d = tok(i + 1), r = tok(i + 2);
                const uint32_t type = ((r >> 28) & 0x7u) | ((r >> 8) & 0x18u);
                if (!(d & 0x80000000u)) return "malformed dcl";
                if (type != 10 && (d & 0x1F) > D3DDECLUSAGE_SAMPLE) return "dcl usage out of range";
                if (vs && type == 1 && ++inputDcls > 16) return "too many vertex inputs";
                first = i + 2; regs = 1;
            }
            else if (op == 81 || op == 48) { if (len != 5) return "malformed def"; regs = 1; }   // def / defi: reg + 4 values
            else if (op == 47) { if (len != 2) return "malformed defb"; regs = 1; }             // defb: reg + 1 value

            for (uint32_t j = 0; j < regs; j++)
                if (!RegisterOk(vs, tok(first + j))) return "register out of range";
            i += 1 + len;
        }
        return "no end token";
    }

    // ---- parsing -------------------------------------------------------------
    enum class Verdict { Accepted, Rejected, Duplicate, Skipped, Absent };

    struct ParseResult
    {
        Verdict     verdict = Verdict::Accepted;
        std::string reason;                 // rejected/skipped: why
        uint32_t    version = 0;
        uint32_t    badKeys = 0, badDecls = 0, badShaders = 0;
        std::string firstBad;               // the first dropped item and why
    };

    // Parse and validate a whole file already in memory. On Rejected/Skipped `out`
    // is left empty. Accepted files may still have had items dropped (bad* counts).
    inline ParseResult Parse(const uint8_t* data, size_t size, CacheContents& out)
    {
        using namespace pipelinekeys;
        ParseResult r;
        out = CacheContents{};
        auto reject = [&](Verdict v, std::string why) { r.verdict = v; r.reason = std::move(why); out = CacheContents{}; return r; };
        auto noteBad = [&](const std::string& what) { if (r.firstBad.empty()) r.firstBad = what; };

        if (size < sizeof(CacheHeader)) return reject(Verdict::Rejected, "too short for a header (" + std::to_string(size) + " bytes)");
        CacheHeader h;
        memcpy(&h, data, sizeof(h));
        if (h.magic != kCacheMagic) return reject(Verdict::Rejected, "not a FusionFix D3D9 cache (wrong magic)");
        r.version = h.version;
        if (h.version > kCacheVersion)
            return reject(Verdict::Skipped, "cache format v" + std::to_string(h.version) + " is newer than this build reads (v" +
                                            std::to_string(kOldestVersion) + "-v" + std::to_string(kCacheVersion) + ")");
        if (h.version < kOldestVersion) return reject(Verdict::Rejected, "cache format v" + std::to_string(h.version) + " does not exist");
        if (h.sectionCount == 0 || h.sectionCount > kMaxSections)
            return reject(Verdict::Rejected, "section count " + std::to_string(h.sectionCount) + " out of range");
        const size_t tableEnd = sizeof(CacheHeader) + (size_t)h.sectionCount * sizeof(CacheSection);
        if (tableEnd > size) return reject(Verdict::Rejected, "truncated: the section table runs past the end");

        const CacheSection* found[6] = {};
        CacheSection secs[kMaxSections];
        memcpy(secs, data + sizeof(CacheHeader), (size_t)h.sectionCount * sizeof(CacheSection));
        for (uint32_t i = 0; i < h.sectionCount; i++)
        {
            const CacheSection& s = secs[i];
            if ((uint64_t)s.offset + s.size > size || s.offset < tableEnd)
                return reject(Verdict::Rejected, "truncated or damaged: section " + std::to_string(s.id) + " (offset " +
                                                 std::to_string(s.offset) + " + " + std::to_string(s.size) +
                                                 ") is outside the file's " + std::to_string(size) + " bytes");
            if (s.id >= 1 && s.id <= 5)
            {
                if (found[s.id]) return reject(Verdict::Rejected, "section " + std::to_string(s.id) + " appears twice");
                found[s.id] = &s;
            }
        }
        if (!found[kSecMeta] || !found[kSecRSTypes] || !found[kSecKeys])
            return reject(Verdict::Rejected, "a required section is missing");

        // Meta: the fixed part, then length-prefixed strings, all inside the section.
        {
            const CacheSection& s = *found[kSecMeta];
            const uint8_t* p = data + s.offset;
            if (s.size < sizeof(CacheMeta)) return reject(Verdict::Rejected, "metadata section too short");
            memcpy(&out.meta, p, sizeof(CacheMeta));
            if (out.meta.stringCount > kMaxMetaStrings) return reject(Verdict::Rejected, "metadata string count out of range");
            size_t at = sizeof(CacheMeta);
            for (uint32_t i = 0; i < out.meta.stringCount; i++)
            {
                uint32_t len = 0;
                if (at + 4 > s.size) return reject(Verdict::Rejected, "metadata strings run past their section");
                memcpy(&len, p + at, 4);
                at += 4;
                if (len > kMaxStringBytes || at + len > s.size) return reject(Verdict::Rejected, "metadata strings run past their section");
                if (i < kMetaStringCount) out.strings[i].assign(reinterpret_cast<const char*>(p + at), len);
                at += len;
            }
        }
        if (out.meta.numRS != kNumRS || out.meta.numSamplers != kNumSamplers)
            return reject(Verdict::Skipped, "records " + std::to_string(out.meta.numRS) + " render states / " +
                                            std::to_string(out.meta.numSamplers) + " samplers, this build " +
                                            std::to_string(kNumRS) + " / " + std::to_string(kNumSamplers));
        {
            const CacheSection& s = *found[kSecRSTypes];
            if (s.count != kNumRS || s.size != kNumRS * 4)
                return reject(Verdict::Skipped, "records a different render-state set");
            out.rsTypes.resize(kNumRS);
            memcpy(out.rsTypes.data(), data + s.offset, kNumRS * 4);
            for (uint32_t i = 0; i < kNumRS; i++)
                if (out.rsTypes[i] != (uint32_t)kTrackedRS[i].rs)
                    return reject(Verdict::Skipped, "records a different render-state set (state " + std::to_string(i) + ")");
        }

        // Declarations. Invalid ones are dropped and the survivors renumbered; keys
        // naming a dropped one are dropped with it.
        std::vector<uint32_t> declRemap;
        if (found[kSecDecls])
        {
            const CacheSection& s = *found[kSecDecls];
            if (s.count > s.size / (4 + sizeof(D3DVERTEXELEMENT9)))
                return reject(Verdict::Rejected, "declaration count does not fit its section");
            declRemap.assign(s.count, kDeclNone);
            size_t at = 0;
            for (uint32_t i = 0; i < s.count; i++)
            {
                uint32_t n = 0;
                if (at + 4 > s.size) return reject(Verdict::Rejected, "declarations run past their section");
                memcpy(&n, data + s.offset + at, 4);
                at += 4;
                if (n == 0 || n > kMaxDeclElements || at + (size_t)n * sizeof(D3DVERTEXELEMENT9) > s.size)
                    return reject(Verdict::Rejected, "declarations run past their section");
                std::vector<D3DVERTEXELEMENT9> d(n);
                memcpy(d.data(), data + s.offset + at, n * sizeof(D3DVERTEXELEMENT9));
                at += n * sizeof(D3DVERTEXELEMENT9);
                if (const char* why = CheckDecl(d.data(), n))
                {
                    r.badDecls++;
                    noteBad("declaration " + std::to_string(i) + ": " + why);
                    continue;
                }
                declRemap[i] = (uint32_t)out.decls.size();
                out.decls.push_back(std::move(d));
            }
        }

        // Keys, widened from v1 if need be.
        {
            const CacheSection& s = *found[kSecKeys];
            const size_t rec = h.version == 1 ? kRecordV1 : sizeof(KeyRecord);
            if ((uint64_t)s.count * rec != s.size)
                return reject(Verdict::Rejected, "key count " + std::to_string(s.count) + " does not match its section's " +
                                                 std::to_string(s.size) + " bytes");
            out.keys.reserve(s.count);
            for (uint32_t i = 0; i < s.count; i++)
            {
                const uint8_t* p = data + s.offset + (size_t)i * rec;
                KeyRecord k{};
                if (h.version == 1)
                {
                    memcpy(&k, p, offsetof(KeyRecord, streamFreq));
                    memcpy(&k.count, p + offsetof(KeyRecord, streamFreq), 2 * sizeof(uint32_t));
                }
                else
                {
                    memcpy(&k, p, sizeof(KeyRecord));
                }
                std::string why;
                if (k.declIndex != kDeclNone)
                {
                    if (k.declIndex >= declRemap.size()) why = "declaration index out of range";
                    else if (declRemap[k.declIndex] == kDeclNone) why = "names a dropped declaration";
                    else k.declIndex = declRemap[k.declIndex];
                }
                if (why.empty()) CheckKey(k, why);
                if (!why.empty())
                {
                    r.badKeys++;
                    noteBad("key " + std::to_string(i) + ": " + why);
                    continue;
                }
                out.keys.push_back(k);
            }
        }

        // Shader bytecode: each blob must hash to the hash it is stored under, and be
        // well-formed SM2/SM3 of the stage it claims.
        if (found[kSecShaders])
        {
            const CacheSection& s = *found[kSecShaders];
            if (s.count > s.size / sizeof(ShaderBlobHeader))
                return reject(Verdict::Rejected, "shader count does not fit its section");
            size_t at = 0;
            for (uint32_t i = 0; i < s.count; i++)
            {
                ShaderBlobHeader bh{};
                if (at + sizeof(bh) > s.size) return reject(Verdict::Rejected, "shaders run past their section");
                memcpy(&bh, data + s.offset + at, sizeof(bh));
                at += sizeof(bh);
                if (bh.size == 0 || bh.size > kMaxShaderBytes || at + bh.size > s.size)
                    return reject(Verdict::Rejected, "shaders run past their section");
                const uint8_t* code = data + s.offset + at;
                at += bh.size;

                const char* why = nullptr;
                if (bh.stage != kStageVS && bh.stage != kStagePS) why = "stage out of range";
                else if (Fnv1a(code, bh.size) != bh.hash) why = "bytecode does not match its hash";
                else why = CheckBytecode(bh.stage, code, bh.size);
                if (why)
                {
                    char hx[20];
                    _snprintf_s(hx, sizeof(hx), _TRUNCATE, "%016llx", (unsigned long long)bh.hash);
                    r.badShaders++;
                    noteBad(std::string("shader ") + hx + ": " + why);
                    continue;
                }
                out.shaders.emplace(bh.hash, ShaderBlob{ bh.stage, std::vector<uint8_t>(code, code + bh.size) });
            }
        }
        return r;
    }

    // ---- one file, start to end ------------------------------------------------
    struct FileResult
    {
        Verdict     verdict = Verdict::Absent;
        std::string reason;   // rejected / skipped: why. duplicate: the file it repeats
        uint64_t    hash = 0;
        size_t      bytes = 0;
        ParseResult parse;
    };

    // Read, hash, de-duplicate, parse and validate. `seen` maps content hashes to the
    // label of the file that brought them; an accepted file is added to it.
    // `shaderDir` is the directory this install resolved ("" or "unknown" if not
    // known): a file captured against another one names shaders this install never
    // creates, so it is skipped.
    inline FileResult LoadFile(const std::wstring& path, const std::string& label, const std::string& shaderDir,
                               std::unordered_map<uint64_t, std::string>& seen, CacheContents& out)
    {
        FileResult f;
        out = CacheContents{};
        std::vector<uint8_t> bytes;
        bool absent = false;
        if (!ReadFileBytes(path, bytes, kMaxFileBytes, f.reason, absent))
        {
            f.verdict = absent ? Verdict::Absent : Verdict::Rejected;
            return f;
        }
        f.bytes = bytes.size();
        f.hash = ContentHash(bytes.data(), bytes.size());
        if (auto it = seen.find(f.hash); it != seen.end())
        {
            f.verdict = Verdict::Duplicate;
            f.reason = it->second;
            return f;
        }
        f.parse = Parse(bytes.data(), bytes.size(), out);
        f.verdict = f.parse.verdict;
        f.reason = f.parse.reason;
        if (f.verdict != Verdict::Accepted) return f;

        const std::string& dir = out.strings[pipelinekeys::kMetaShaderDir];
        if (!dir.empty() && dir != "unknown" && !shaderDir.empty() && shaderDir != "unknown" && dir != shaderDir)
        {
            f.verdict = Verdict::Skipped;
            f.reason = "captured against shader directory '" + Printable(dir) + "', this install uses '" + shaderDir + "'";
            out = CacheContents{};
            return f;
        }
        seen.emplace(f.hash, label);
        return f;
    }

    // "accepted - ..." / "rejected - ..." etc., for the one line each file gets.
    inline std::string Describe(const FileResult& f, const CacheContents& c)
    {
        switch (f.verdict)
        {
        case Verdict::Absent:    return "not present";
        case Verdict::Rejected:  return "rejected - " + f.reason;
        case Verdict::Skipped:   return "skipped - " + f.reason;
        case Verdict::Duplicate: return "duplicate - same content as " + f.reason + ", used once";
        default: break;
        }
        char buf[512];
        const auto& m = c.meta;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "accepted - v%u%s, %zu keys, %zu declarations, %zu shaders; captured on %s / %s / %s, f%u-ms%d, %s",
                    f.parse.version, f.parse.version == 1 ? " (upgraded in memory)" : "",
                    c.keys.size(), c.decls.size(), c.shaders.size(),
                    Printable(c.strings[pipelinekeys::kMetaOS]).c_str(),
                    Printable(c.strings[pipelinekeys::kMetaAdapter]).c_str(),
                    m.backend == 1 ? "DXVK" : "native D3D9", m.bbFormat, m.msaa,
                    Printable(c.strings[pipelinekeys::kMetaShaderDir]).c_str());
        std::string s = buf;
        const auto& p = f.parse;
        if (p.badKeys || p.badDecls || p.badShaders)
            s += "; dropped " + std::to_string(p.badKeys) + " invalid keys, " + std::to_string(p.badDecls) +
                 " declarations, " + std::to_string(p.badShaders) + " shaders (first: " + p.firstBad + ")";
        return s;
    }

    // ---- the drop-in folder ------------------------------------------------------
    struct DropIns
    {
        std::vector<std::wstring> files;      // top-level .bin files, full paths, sorted by name
        std::vector<std::wstring> other;      // top-level files that are neither .bin nor .foz (names)
        uint32_t     subdirs = 0;
        std::wstring firstSubdir;
        uint32_t     foz = 0;                 // Vulkan .foz files (they belong in pipelinecache)
        std::wstring firstFoz;
        uint32_t     misplacedBins = 0;       // .bin files in plugins\pipelinecache\ (top level)
        std::wstring firstMisplaced;
    };

    // plugins\d3d9cache\, top level only. Also counts the .bin files sitting in
    // plugins\pipelinecache\, which is where drop-ins lived before this folder.
    inline DropIns FindDropIns()
    {
        DropIns d;
        const std::wstring dir = DropInDir();
        if (dir.empty()) return d;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (!d.subdirs++) d.firstSubdir = name;
                }
                else if (EndsWithNoCase(name, L".bin")) d.files.push_back(dir + name);
                else if (EndsWithNoCase(name, L".foz")) { if (!d.foz++) d.firstFoz = name; }
                else d.other.push_back(name);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(d.files.begin(), d.files.end());
        std::sort(d.other.begin(), d.other.end());

        const std::wstring vk = PluginsDir() + L"pipelinecache\\";
        h = FindFirstFileW((vk + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && EndsWithNoCase(name, L".bin"))
                    if (!d.misplacedBins++) d.firstMisplaced = name;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return d;
    }

    // ---- this PC's own snapshot --------------------------------------------------
    // Content hash of each own capture file as it was at launch, by lower-case file
    // name: the replay treats a drop-in with that content as this file, even after
    // the capture has since rewritten it.
    inline std::unordered_map<std::wstring, uint64_t>& OwnLaunchHashes()
    {
        static std::unordered_map<std::wstring, uint64_t> m;
        return m;
    }

    inline std::wstring Lower(std::wstring s)
    {
        for (auto& ch : s) ch = (wchar_t)towlower(ch);
        return s;
    }

    // Which snapshots this PC wrote, so it can delete its own previous one and never
    // anything else. One name per line; lines that are not a content name are ignored.
    inline std::wstring StatePath() { return PluginsDir() + L"FusionFix.d3d9cache.state"; }

    inline std::vector<std::string> ReadState()
    {
        std::vector<std::string> names;
        std::vector<uint8_t> bytes;
        std::string why;
        bool absent = false;
        if (!ReadFileBytes(StatePath(), bytes, 64 * 1024, why, absent)) return names;
        std::string line;
        for (size_t i = 0; i <= bytes.size(); i++)
        {
            const char ch = i < bytes.size() ? (char)bytes[i] : '\n';
            if (ch == '\r') continue;
            if (ch != '\n') { line += ch; continue; }
            if (IsContentName(line) && std::find(names.begin(), names.end(), line) == names.end())
                names.push_back(line);
            line.clear();
        }
        return names;
    }

    // Write `data` to `path` through a temp file beside the ASI and a rename, so a
    // crash leaves either nothing or a whole file, and never a stray temp in a folder
    // the player copies around. `replace` = overwrite an existing target.
    inline bool WriteViaTemp(const std::wstring& path, const void* data, size_t size, bool replace)
    {
        const std::wstring tmp = PluginsDir() + L"FusionFix.d3d9cache.tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0;
        const bool ok = WriteFile(h, data, (DWORD)size, &put, nullptr) && put == size;
        CloseHandle(h);
        if (ok && MoveFileExW(tmp.c_str(), path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) return true;
        DeleteFileW(tmp.c_str());
        return false;
    }

    // Once per launch, before capture writes anything: copy each of this PC's own
    // capture files (FusionFix.pipelinecache.f<fmt>-ms<msaa>.bin) into
    // plugins\d3d9cache\ as FusionFix.<h>.bin, skip names that already exist, and
    // delete the snapshots this PC wrote before that are no longer current. A file
    // is only shared if it reads back as valid here. `log` gets one line per action.
    template <typename LogFn>
    inline void SnapshotOwnCaptures(LogFn log)
    {
        const std::wstring plugins = PluginsDir();
        if (plugins.empty()) return;
        const std::wstring dir = DropInDir();

        std::vector<std::wstring> own;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((plugins + L"FusionFix.pipelinecache.f*.bin").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = fd.cFileName;
                // FindFirstFile also matches 8.3 aliases; insist on the real shape.
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Lower(name).rfind(L"fusionfix.pipelinecache.f", 0) == 0 &&
                    EndsWithNoCase(name, L".bin") && name.find(L"-ms") != std::wstring::npos)
                    own.push_back(name);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(own.begin(), own.end());

        const std::vector<std::string> previous = ReadState();
        std::vector<std::string> current;
        bool failed = false;
        for (const auto& name : own)
        {
            std::vector<uint8_t> bytes;
            std::string why;
            bool absent = false;
            if (!ReadFileBytes(plugins + name, bytes, kMaxFileBytes, why, absent))
            {
                log(("snapshot: " + Utf8(name) + " " + why + " - not shared").c_str());
                continue;
            }
            const uint64_t hash = ContentHash(bytes.data(), bytes.size());
            OwnLaunchHashes()[Lower(name)] = hash;
            CacheContents c;
            ParseResult p = Parse(bytes.data(), bytes.size(), c);
            if (p.verdict != Verdict::Accepted)
            {
                log(("snapshot: " + Utf8(name) + " does not read back as valid (" + p.reason + ") - not shared").c_str());
                continue;
            }
            const std::string snap = ContentName(hash);
            if (std::find(current.begin(), current.end(), snap) == current.end()) current.push_back(snap);
            const std::wstring target = dir + Wide(snap);
            if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                log(("snapshot: " + Utf8(name) + " is already in d3d9cache\\ as " + snap).c_str());
                continue;
            }
            CreateDirectoryW(dir.c_str(), nullptr);
            if (WriteViaTemp(target, bytes.data(), bytes.size(), false))
                log(("snapshot: " + Utf8(name) + " -> d3d9cache\\" + snap + " (" + std::to_string(bytes.size()) +
                     " bytes) - copy plugins\\d3d9cache\\ to your other PCs").c_str());
            else
            {
                failed = true;
                current.pop_back();
                log(("snapshot: could not write d3d9cache\\" + snap + " (error " + std::to_string(GetLastError()) + ")").c_str());
            }
        }

        // Retire this PC's earlier snapshots. If a write failed, keep them: the old
        // one is still the best this PC has to share.
        std::vector<std::string> keep = current;
        for (const auto& old : previous)
        {
            if (std::find(current.begin(), current.end(), old) != current.end()) continue;
            if (failed) { keep.push_back(old); continue; }
            const std::wstring p = dir + Wide(old);
            if (DeleteFileW(p.c_str()))
                log(("snapshot: removed this PC's previous snapshot " + old).c_str());
            else if (GetLastError() != ERROR_FILE_NOT_FOUND)
                keep.push_back(old);
        }

        if (keep != previous)
        {
            std::string text;
            for (const auto& n : keep) text += n + "\r\n";
            if (keep.empty()) DeleteFileW(StatePath().c_str());
            else if (!WriteViaTemp(StatePath(), text.data(), text.size(), true))
                log("snapshot: could not write FusionFix.d3d9cache.state");
        }
    }

    // ---- the running game ----------------------------------------------------------
    // GTA IV stores the shader directory it resolved (win32_30, win32_30_nv8, ...) in
    // a char* global, chosen by probing depth formats. Read defensively: a wrong
    // build would give a wild pointer, and "unknown" is better than a plausible lie.
    inline bool ReadShaderDirGlobal(char* out, size_t n)
    {
        const uintptr_t kShaderDirPtr = 0x01633800;   // GTA IV 1.2.0.59
        __try
        {
            auto base = (uintptr_t)GetModuleHandleW(nullptr);
            const char* s = *(const char* const*)(base + (kShaderDirPtr - 0x400000));
            if (s && !IsBadStringPtrA(s, 64) && strncmp(s, "win32_", 6) == 0)
            {
                strncpy_s(out, n, s, _TRUNCATE);
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    inline std::string ActiveShaderDir()
    {
        char buf[64] = {};
        return ReadShaderDirGlobal(buf, sizeof(buf)) ? std::string(buf) : std::string("unknown");
    }
}
