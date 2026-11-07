// Retail texture VA vault from NpcProxy (gfx.cpp section 9), sans graphics mods.
// No RTX Remix, no LOD/filter/AA changes, and no new game/asset loader.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <new>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <intrin.h>
#include "retail_texture_vault.h"
#include "d3d9device.h"

namespace {
enum { kStages = 21, kMaxLevels = 15 };
const DWORD kSettleMs = 3000;
const LONG kSweepBudget = 8 * 1024 * 1024;
const LONG kPressureSweepBudget = 16 * 1024 * 1024;
const unsigned __int64 kLowVaThreshold = 768ull * 1024 * 1024;
const unsigned kKeepHotAfter = 4;
const unsigned kMinTextureBytes = 96 * 1024;
const unsigned __int64 kMaxTextureBytes = 256ull * 1024 * 1024;
// Direct3D9 callsite in the game's native texture class.
const unsigned char kCreateBytes[10] = { 0x51,0x0f,0xb7,0x4f,0x34,0x51,0x56,0xff,0x50,0x5c };
const uintptr_t kCreateInstruction = 0x0068D690;
const uintptr_t kTextureReturnAddress = 0x0068D69A;

class VaultTexture;
CRITICAL_SECTION g_cs;
INIT_ONCE g_initOnce=INIT_ONCE_STATIC_INIT;
volatile LONG g_initState; // published only after critical section initialized
BOOL CALLBACK InitVaultCriticalSection(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_cs);
    InterlockedExchange(&g_initState,1);
    return TRUE;
}
IDirect3DDevice9* g_dev;
ProxyIDirect3DDevice9* g_owner;
void* g_vaultVtbl;
VaultTexture* g_head;
bool g_enabled;
bool g_verified;
bool g_monitor;
char g_monitorPath[MAX_PATH];
DWORD g_lastMonitor;
unsigned g_count, g_relocks, g_natives, g_failures;
unsigned __int64 g_hotBytes, g_coldBytes;
DWORD g_frame;
unsigned g_usedVaMb, g_peakVaMb;
IDirect3DBaseTexture9* g_stageTex[kStages]; // observer only, never AddRef
VaultTexture* g_stageVault[kStages];

struct Guard {
    Guard() { EnterCriticalSection(&g_cs); }
    ~Guard() { LeaveCriticalSection(&g_cs); }
};

int StageIndex(DWORD stage) {
    if (stage < 16) return (int)stage;
    if (stage == D3DDMAPSAMPLER) return 16;
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
        return 17 + (int)(stage - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}
DWORD StageId(int i) {
    if (i < 16) return (DWORD)i;
    if (i == 16) return D3DDMAPSAMPLER;
    return D3DVERTEXTEXTURESAMPLER0 + (DWORD)(i - 17);
}
bool RetailSignatureMatches() {
    __try {
        if ((uintptr_t)GetModuleHandleA(NULL) != (uintptr_t)0x00400000) return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)kCreateInstruction, &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            (uintptr_t)mbi.BaseAddress + mbi.RegionSize < kCreateInstruction + sizeof(kCreateBytes))
            return false;
        return memcmp((const void*)kCreateInstruction, kCreateBytes, sizeof(kCreateBytes)) == 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int BlockFormat(D3DFORMAT f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Bytes of one level, or 0 for a format this does not take.
unsigned LevelBytes(D3DFORMAT f, unsigned w, unsigned h)
{
    unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (!bw) bw = 1;
    if (!bh) bh = 1;
    switch (f) {
    case D3DFMT_DXT1:                                               return bw * bh * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return bw * bh * 16;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:                     return w * h * 4;
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:                         return w * h * 2;
    case D3DFMT_A8: case D3DFMT_L8:                                 return w * h;
    default:                                                        return 0;
    }
}

unsigned FullChain(unsigned w, unsigned h)
{
    unsigned n = 1;
    while (w > 1 || h > 1) { w >>= 1; h >>= 1; ++n; }
    return n;
}

void CopyRows(void* dst, int dstPitch, const void* src, int srcPitch, unsigned rows)
{
    unsigned r;
    size_t n = (size_t)(dstPitch < srcPitch ? dstPitch : srcPitch);
    if (dstPitch == srcPitch) { memcpy(dst, src, n * rows); return; }
    for (r = 0; r < rows; ++r)
        memcpy((unsigned char*)dst + (size_t)r * dstPitch, (const unsigned char*)src + (size_t)r * srcPitch, n);
}

int IsVault(IDirect3DBaseTexture9* t)
{
    return t && g_vaultVtbl && *(void**)t == g_vaultVtbl;
}
// ===========================================================================
// A texture of the game's whose second copy is looked after here.
//
// To the game it is the managed texture it asked for. Behind it:
//   m_real   what is drawn with. A default-pool texture: Direct3D keeps no
//            copy of those in the program's memory.
//   m_stage  a system-memory texture the game's locks are answered from, kept
//            only while the game is filling the texture or has just read it.
//   m_section the same bytes in memory the program holds by a handle and has
//            not mapped: it is the program's, and it takes none of the
//            address space. From there the texture is put back after the
//            device is reset, and the game is answered if it ever asks to
//            read or write the texture again.
// ===========================================================================
class VaultTexture : public IDirect3DTexture9 {
public:
    LONG               m_ref;
    IDirect3DTexture9* m_real;
    IDirect3DTexture9* m_stage;
    HANDLE             m_section;
    UINT               m_w, m_h, m_levels;
    D3DFORMAT          m_fmt;
    DWORD              m_bytes;
    DWORD              m_off[kMaxLevels];
    int                m_pitch[kMaxLevels];
    unsigned           m_rows[kMaxLevels];
    unsigned char      m_locked[kMaxLevels];
    unsigned char      m_readOnly[kMaxLevels];
    unsigned char      m_wrote[kMaxLevels];     // written since the copy outside was made
    unsigned char      m_up[kMaxLevels];        // m_real has this level as the game last left it
    unsigned           m_notUp;
    unsigned char      m_native;                // handed back: m_real is a managed texture and this only passes calls on
    unsigned char      m_keepHot;
    unsigned char      m_inVault;               // m_section holds every level
    DWORD              m_lastLock;
    unsigned           m_relocks;
    DWORD              m_priority;
    VaultTexture*      m_prev;
    VaultTexture*      m_next;

    VaultTexture(IDirect3DTexture9* real, IDirect3DTexture9* stage, UINT w, UINT h, UINT levels, D3DFORMAT fmt)
        : m_ref(1), m_real(real), m_stage(stage), m_section(0), m_w(w), m_h(h), m_levels(levels),
          m_fmt(fmt), m_bytes(0), m_notUp(levels), m_native(0), m_keepHot(0), m_inVault(0),
          m_lastLock(GetTickCount()), m_relocks(0), m_priority(0), m_prev(0), m_next(0)
    {
        memset(m_off, 0, sizeof(m_off));
        memset(m_pitch, 0, sizeof(m_pitch));
        memset(m_rows, 0, sizeof(m_rows));
        memset(m_locked, 0, sizeof(m_locked));
        memset(m_readOnly, 0, sizeof(m_readOnly));
        memset(m_wrote, 0, sizeof(m_wrote));
        memset(m_up, 0, sizeof(m_up));
        g_vaultVtbl = *(void**)this;
    }

    unsigned LevelW(unsigned l) const { unsigned v = m_w >> l; return v ? v : 1; }
    unsigned LevelH(unsigned l) const { unsigned v = m_h >> l; return v ? v : 1; }
    int AnyLocked() const
    {
        unsigned l;
        for (l = 0; l < m_levels; ++l) if (m_locked[l]) return 1;
        return 0;
    }

    // Learns how Direct3D lays each level out, and clears the new copy.
    int Probe()
    {
        unsigned l;
        DWORD off = 0;
        for (l = 0; l < m_levels; ++l) {
            D3DLOCKED_RECT lr;
            if (FAILED(m_stage->LockRect(l, &lr, 0, D3DLOCK_NO_DIRTY_UPDATE)) || !lr.pBits || lr.Pitch <= 0)
                return 0;
            m_pitch[l] = lr.Pitch;
            m_rows[l] = BlockFormat(m_fmt) ? (LevelH(l) + 3) / 4 : LevelH(l);
            if (!m_rows[l]) m_rows[l] = 1;
            m_off[l] = off;
            memset(lr.pBits, 0, (size_t)lr.Pitch * m_rows[l]);
            off += (DWORD)lr.Pitch * m_rows[l];
            m_stage->UnlockRect(l);
        }
        m_bytes = off;
        return off != 0;
    }

    int Upload(unsigned l)
    {
        IDirect3DSurface9* s = 0;
        IDirect3DSurface9* d = 0;
        int ok = 0;
        if (SUCCEEDED(m_stage->GetSurfaceLevel(l, &s)) && s &&
            SUCCEEDED(m_real->GetSurfaceLevel(l, &d)) && d)
            ok = SUCCEEDED(g_dev->UpdateSurface(s, 0, d, 0));
        if (s) s->Release();
        if (d) d->Release();
        return ok;
    }

    void UploadLevel(unsigned l)
    {
        if (Upload(l)) {
            if (!m_up[l]) { m_up[l] = 1; --m_notUp; }
        } else {
            ++g_failures;
            GoNative();         // Direct3D manages it itself from here on, as the game asked
        }
    }

    // Gives m_real every level it does not have as the game last left it.
    void Seed()
    {
        unsigned l;
        for (l = 0; l < m_levels && !m_native && m_real && m_stage; ++l)
            if (!m_up[l] && !m_locked[l]) UploadLevel(l);
    }

    // The copy inside the address space, made again from the one outside.
    int MakeStage()
    {
        IDirect3DTexture9* t = 0;
        unsigned l;
        if (m_stage) return 1;
        if (FAILED(g_dev->CreateTexture(m_w, m_h, m_levels, 0, m_fmt, D3DPOOL_SYSTEMMEM, &t, 0)) || !t)
            return 0;
        if (m_inVault) {
            const unsigned char* view = (const unsigned char*)MapViewOfFile(m_section, FILE_MAP_READ, 0, 0, m_bytes);
            int ok = view != 0;
            for (l = 0; ok && l < m_levels; ++l) {
                D3DLOCKED_RECT lr;
                if (FAILED(t->LockRect(l, &lr, 0, D3DLOCK_NO_DIRTY_UPDATE)) || !lr.pBits) { ok = 0; break; }
                CopyRows(lr.pBits, lr.Pitch, view + m_off[l], m_pitch[l], m_rows[l]);
                t->UnlockRect(l);
            }
            if (view) UnmapViewOfFile(view);
            if (!ok) { t->Release(); return 0; }
            g_coldBytes -= m_bytes;
        }
        g_hotBytes += m_bytes;
        m_stage = t;
        return 1;
    }

    // The copy inside goes; the one outside is brought up to date first.
    void Cool()
    {
        unsigned char* view;
        unsigned l;
        int ok = 1;
        if (m_native || !m_stage || AnyLocked()) return;
        if (m_real && m_notUp) Seed();
        if (m_native || !m_stage) return;
        if (!m_section) {
            m_section = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, m_bytes, 0);
            if (!m_section) { m_keepHot = 1; ++g_failures; return; }
        }
        view = (unsigned char*)MapViewOfFile(m_section, FILE_MAP_WRITE, 0, 0, m_bytes);
        if (!view) { m_lastLock = GetTickCount(); return; }     // no room for the view just now: later
        for (l = 0; l < m_levels; ++l) {
            D3DLOCKED_RECT lr;
            if (m_inVault && !m_wrote[l]) continue;
            if (FAILED(m_stage->LockRect(l, &lr, 0, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE)) || !lr.pBits) {
                ok = 0;
                break;
            }
            CopyRows(view + m_off[l], m_pitch[l], lr.pBits, lr.Pitch, m_rows[l]);
            m_stage->UnlockRect(l);
        }
        UnmapViewOfFile(view);
        if (!ok) { m_keepHot = 1; ++g_failures; return; }
        m_inVault = 1;
        memset(m_wrote, 0, sizeof(m_wrote));
        m_stage->Release();
        m_stage = 0;
        g_hotBytes -= m_bytes;
        g_coldBytes += m_bytes;
    }

    // After the device was reset: a new texture to draw with, filled again.
    void Restore()
    {
        IDirect3DTexture9* r = 0;
        if (m_native || m_real) return;
        if (FAILED(g_dev->CreateTexture(m_w, m_h, m_levels, 0, m_fmt, D3DPOOL_DEFAULT, &r, 0)) || !r) return;
        m_real = r;
        memset(m_up, 0, sizeof(m_up));
        m_notUp = m_levels;
        if (!MakeStage()) {
            r->Release();
            m_real = 0;
            ++g_failures;
            return;
        }
        m_lastLock = GetTickCount();
        Seed();
    }

    void Ready()
    {
        if (m_native) return;
        if (!m_real) Restore();
        else if (m_notUp && m_stage) Seed();
    }

    // Hands the texture back: from here on it is the managed texture the game
    // asked for and this object only passes the game's calls on.
    int GoNative()
    {
        IDirect3DTexture9* t = 0;
        unsigned l;
        int i;
        if (m_native) return 1;
        if (AnyLocked()) return 0;
        if (!MakeStage()) return 0;
        if (FAILED(g_dev->CreateTexture(m_w, m_h, m_levels, 0, m_fmt, D3DPOOL_MANAGED, &t, 0)) || !t) return 0;
        for (l = 0; l < m_levels; ++l) {
            D3DLOCKED_RECT a, b;
            if (FAILED(m_stage->LockRect(l, &a, 0, D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE))) continue;
            if (SUCCEEDED(t->LockRect(l, &b, 0, 0))) {
                CopyRows(b.pBits, b.Pitch, a.pBits, a.Pitch, m_rows[l]);
                t->UnlockRect(l);
            }
            m_stage->UnlockRect(l);
        }
        for (i = 0; i < kStages; ++i)
            if (g_stageVault[i] == this) g_dev->SetTexture(StageId(i), t);
        if (m_real) m_real->Release();
        m_real = t;
        m_stage->Release();
        m_stage = 0;
        g_hotBytes -= m_bytes;
        if (m_section) { CloseHandle(m_section); m_section = 0; }
        m_inVault = 0;
        m_native = 1;
        --g_count;
        ++g_natives;
        return 1;
    }

    // ---- IUnknown ----
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
            riid == __uuidof(IDirect3DBaseTexture9) || riid == __uuidof(IDirect3DTexture9)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = 0;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG __stdcall Release() override;

    // ---- IDirect3DResource9 ----
    HRESULT __stdcall GetDevice(IDirect3DDevice9** ppDevice) override
    {
        if (!ppDevice || !g_owner) return D3DERR_INVALIDCALL;
        *ppDevice = g_owner;
        g_owner->AddRef();
        return D3D_OK;
    }
    HRESULT __stdcall SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override
    {
        return m_real ? m_real->SetPrivateData(guid, data, size, flags) : D3DERR_INVALIDCALL;
    }
    HRESULT __stdcall GetPrivateData(REFGUID guid, void* data, DWORD* size) override
    {
        return m_real ? m_real->GetPrivateData(guid, data, size) : D3DERR_NOTFOUND;
    }
    HRESULT __stdcall FreePrivateData(REFGUID guid) override
    {
        return m_real ? m_real->FreePrivateData(guid) : D3DERR_NOTFOUND;
    }
    DWORD __stdcall SetPriority(DWORD p) override
    {
        DWORD old = m_priority;
        if (m_native && m_real) return m_real->SetPriority(p);
        m_priority = p;
        return old;
    }
    DWORD __stdcall GetPriority() override
    {
        return (m_native && m_real) ? m_real->GetPriority() : m_priority;
    }
    void __stdcall PreLoad() override
    {
        if (m_native && m_real) m_real->PreLoad();
    }
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_TEXTURE; }

    // ---- IDirect3DBaseTexture9 ----
    DWORD __stdcall SetLOD(DWORD lod) override
    {
        // Only a managed texture has this. Asked for anything but the default,
        // the texture is handed back so that Direct3D does exactly what it would.
        if (!m_native) {
            if (!lod) return 0;
            Guard g;
            GoNative();
        }
        return (m_native && m_real) ? m_real->SetLOD(lod) : 0;
    }
    DWORD __stdcall GetLOD() override { return (m_native && m_real) ? m_real->GetLOD() : 0; }
    DWORD __stdcall GetLevelCount() override { return m_levels; }
    HRESULT __stdcall SetAutoGenFilterType(D3DTEXTUREFILTERTYPE f) override
    {
        return m_real ? m_real->SetAutoGenFilterType(f) : D3D_OK;
    }
    D3DTEXTUREFILTERTYPE __stdcall GetAutoGenFilterType() override
    {
        return m_real ? m_real->GetAutoGenFilterType() : D3DTEXF_LINEAR;
    }
    void __stdcall GenerateMipSubLevels() override {}

    // ---- IDirect3DTexture9 ----
    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC* d) override
    {
        if (!d || level >= m_levels) return D3DERR_INVALIDCALL;
        d->Format = m_fmt;
        d->Type = D3DRTYPE_SURFACE;
        d->Usage = 0;
        d->Pool = D3DPOOL_MANAGED;
        d->MultiSampleType = D3DMULTISAMPLE_NONE;
        d->MultiSampleQuality = 0;
        d->Width = LevelW(level);
        d->Height = LevelH(level);
        return D3D_OK;
    }
    HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface9** ppSurface) override
    {
        // The game's own class never asks this of a texture of this kind. For
        // whoever does, the surface must be one that can be locked and that
        // knows its texture: Direct3D's own, of a texture it manages.
        Guard g;
        if (!m_native) GoNative();
        if (!m_real) return D3DERR_INVALIDCALL;
        return m_real->GetSurfaceLevel(level, ppSurface);
    }
    HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT* lr, const RECT* rc, DWORD flags) override
    {
        HRESULT hr;
        Guard g;
        if (m_native) return m_real ? m_real->LockRect(level, lr, rc, flags) : D3DERR_INVALIDCALL;
        if (level >= m_levels || !lr || m_locked[level]) return D3DERR_INVALIDCALL;
        if (!m_stage) {
            if (!MakeStage()) { ++g_failures; return E_OUTOFMEMORY; }
            ++m_relocks;
            ++g_relocks;
            if (m_relocks >= kKeepHotAfter) m_keepHot = 1;
        }
        hr = m_stage->LockRect(level, lr, rc, flags);
        if (FAILED(hr)) return hr;
        m_locked[level] = 1;
        m_readOnly[level] = (flags & D3DLOCK_READONLY) ? 1 : 0;
        m_lastLock = GetTickCount();
        return hr;
    }
    HRESULT __stdcall UnlockRect(UINT level) override
    {
        HRESULT hr;
        Guard g;
        if (m_native) return m_real ? m_real->UnlockRect(level) : D3DERR_INVALIDCALL;
        if (level >= m_levels || !m_locked[level] || !m_stage) return D3DERR_INVALIDCALL;
        hr = m_stage->UnlockRect(level);
        m_locked[level] = 0;
        m_lastLock = GetTickCount();
        if (!m_readOnly[level]) {
            m_wrote[level] = 1;
            if (m_up[level]) { m_up[level] = 0; ++m_notUp; }
            if (m_real) UploadLevel(level);
        }
        return hr;
    }
    HRESULT __stdcall AddDirtyRect(const RECT* rc) override
    {
        // Every level goes to the card whole each time it is unlocked.
        return (m_native && m_real) ? m_real->AddDirtyRect(rc) : D3D_OK;
    }
};

ULONG __stdcall VaultTexture::Release()
{
    LONG r = InterlockedDecrement(&m_ref);
    int i;
    if (r != 0) return (ULONG)r;
    EnterCriticalSection(&g_cs);
    if (m_prev) m_prev->m_next = m_next; else if (g_head == this) g_head = m_next;
    if (m_next) m_next->m_prev = m_prev;
    for (i = 0; i < kStages; ++i)
        if (g_stageVault[i] == this) {      // Direct3D still holds what it is drawing with
            g_stageVault[i] = 0;
            g_stageTex[i] = 0;
        }
    if (!m_native) {
        if (m_stage) g_hotBytes -= m_bytes; else g_coldBytes -= m_bytes;
        --g_count;
    }
    if (m_stage) m_stage->Release();
    if (m_real) m_real->Release();
    if (m_section) CloseHandle(m_section);
    LeaveCriticalSection(&g_cs);
    delete this;
    return 0;
}

} // namespace

void OCV_OnDeviceCreated(ProxyIDirect3DDevice9* owner, IDirect3DDevice9* real) {
    if (!real) return;
    if (!InitOnceExecuteOnce(&g_initOnce,InitVaultCriticalSection,NULL,NULL) || !g_initState)
        return;
    Guard g;
    if (g_dev) return; // NpcProxy supports one device; do not mix resource owners.
    g_dev = real;
    g_owner = owner;
    char path[MAX_PATH]={0}, ini[MAX_PATH]={0};
    DWORD n=GetModuleFileNameA(NULL,path,MAX_PATH);
    if (n && n<MAX_PATH) {
        char* slash=strrchr(path,'\\');
        if (slash) {
            slash[1]=0;
            if (_snprintf_s(ini,sizeof(ini),_TRUNCATE,"%sproxy.ini",path)>=0) {
                g_enabled = GetPrivateProfileIntA("WorldResourceMemory","TextureVault",0,ini)!=0;
                g_monitor = GetPrivateProfileIntA("WorldResourceMemory","Monitor",0,ini)!=0;
                if (g_monitor) {
                    if (_snprintf_s(g_monitorPath,sizeof(g_monitorPath),_TRUNCATE,
                                    "%sworld_resource_memory.csv",path)<0)
                        g_monitor=false;
                }
            }
        }
    }
    g_verified = g_enabled && RetailSignatureMatches();
    if (!g_verified) g_enabled = false;
}
void OCV_OnDeviceReleased(IDirect3DDevice9* real) {
    if (!g_initState || real != g_dev) return;
    Guard g;
    g_enabled=false;
    g_verified=false;
    g_dev=0;
    g_owner=0;
    memset(g_stageTex,0,sizeof(g_stageTex));
    memset(g_stageVault,0,sizeof(g_stageVault));
}
void OCV_BeforeReset(IDirect3DDevice9* real) {
    if (!g_initState || real != g_dev) return;
    Guard g;
    // Must unbind every DEFAULT texture before Reset. Originals remain
    // available in hot staging or the unmapped section for reconstruction.
    for (int i=0;i<kStages;++i) {
        if (g_stageVault[i]) {
            real->SetTexture(StageId(i),0);
            g_stageVault[i]=0;
            g_stageTex[i]=0;
        }
    }
    for (int i=0;i<kStages;++i) {
        IDirect3DBaseTexture9* t=0;
        if (FAILED(real->GetTexture(StageId(i),&t)) || !t) continue;
        for (VaultTexture* v=g_head;v;v=v->m_next) {
            if (!v->m_native && v->m_real==t) {
                real->SetTexture(StageId(i),0);
                g_stageTex[i]=0;
                break;
            }
        }
        t->Release();
    }
    for (VaultTexture* v=g_head;v;v=v->m_next) {
        if (v->m_native || !v->m_real) continue;
        v->m_real->Release();
        v->m_real=0;
        memset(v->m_up,0,sizeof(v->m_up));
        v->m_notUp=v->m_levels;
    }
}
void OCV_AfterReset(IDirect3DDevice9*, HRESULT) {
    // Lazy Restore() on next SetTexture. A failed reset does not invalidate
    // backing sections. No mutable state depends on the reset result.
}
void OCV_OnPresent(IDirect3DDevice9* real) {
    if (!g_initState || real != g_dev) return;
    if ((g_frame++ % 30)==0) {
        MEMORYSTATUSEX s={0};
        s.dwLength=sizeof(s);
        if (GlobalMemoryStatusEx(&s)) {
            g_usedVaMb=(unsigned)((s.ullTotalVirtual-s.ullAvailVirtual)>>20);
            if (g_usedVaMb>g_peakVaMb) g_peakVaMb=g_usedVaMb;
        }
    }
    if (!g_enabled && !g_monitor) return;
    Guard g;
    DWORD tick=GetTickCount();
    if (g_monitor && (DWORD)(tick-g_lastMonitor)>=1000u) {
        g_lastMonitor=tick;
        FILE* output=fopen(g_monitorPath,"ab+");
        if (output) {
            fseek(output,0,SEEK_END);
            if (ftell(output)==0)
                fputs("time_ms,used_va_mib,peak_va_mib,cold_texture_mib,hot_texture_mib,vault_count,failures,texture_vault_enabled,approx_gpu_available_mib\n",output);
            // GetAvailableTextureMem is only a driver hint, not a VRAM budget.
            fprintf(output,"%lu,%u,%u,%u,%u,%u,%u,%u,%u\n",
                    (unsigned long)tick,g_usedVaMb,g_peakVaMb,
                    (unsigned)(g_coldBytes>>20),(unsigned)(g_hotBytes>>20),
                    g_count,g_failures,g_enabled?1u:0u,
                    (unsigned)(real->GetAvailableTextureMem()>>20));
            fclose(output);
        }
    }
    if (!g_enabled) return;
    DWORD now=GetTickCount();
    LONG budget=kSweepBudget;
    for (VaultTexture* v=g_head;v && budget>0;v=v->m_next) {
        if (!v->m_stage || v->m_native || v->m_keepHot || v->AnyLocked()) continue;
        if (now-v->m_lastLock < kSettleMs) continue;
        budget-=(LONG)v->m_bytes;
        v->Cool();
    }
}
HRESULT OCV_CreateTexture(IDirect3DDevice9* real, UINT w, UINT h, UINT levels, DWORD usage,
                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out,
                          HANDLE* shared, void* returnAddress) {
    if (!g_initState || real!=g_dev || !g_enabled || !g_verified) return S_FALSE;
    if (pool!=D3DPOOL_MANAGED || usage!=0 || shared || !out || !w || !h) return S_FALSE;
    if ((uintptr_t)returnAddress!=kTextureReturnAddress) return S_FALSE;
    if (LevelBytes(fmt,w,h)<kMinTextureBytes) return S_FALSE;
    const unsigned full=FullChain(w,h);
    if (!levels) levels=full;
    if (levels>full || levels>kMaxLevels || w>8192 || h>8192) return S_FALSE;
    // Size arithmetic must be bounded before Probe accumulates DWORD offsets.
    // Fail open to native D3D9; never wrap a texture with overflowing levels.
    unsigned __int64 byteEstimate=0;
    for (unsigned l=0;l<levels;++l) {
        const unsigned lw=(w>>l)?(w>>l):1;
        const unsigned lh=(h>>l)?(h>>l):1;
        byteEstimate+=LevelBytes(fmt,lw,lh);
        if (byteEstimate>kMaxTextureBytes) return S_FALSE;
    }
    Guard g;
    // A second world can start allocating textures while rendering is paused,
    // so Present() may not sweep the first world's cold staging copies.
    // Opportunistically cool already-idle copies before the new allocation.
    // Limit work per request and use only the current D3D call thread.
    MEMORYSTATUSEX memoryStatus={0};
    memoryStatus.dwLength=sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) &&
        memoryStatus.ullAvailVirtual < kLowVaThreshold) {
        const DWORD now=GetTickCount();
        LONG budget=kPressureSweepBudget;
        for (VaultTexture* old=g_head;old && budget>0;old=old->m_next) {
            if (!old->m_stage || old->m_native || old->m_keepHot || old->AnyLocked()) continue;
            if (now-old->m_lastLock < kSettleMs) continue;
            budget-=(LONG)old->m_bytes;
            old->Cool();
        }
    }
    IDirect3DTexture9* gpu=0;
    if (FAILED(real->CreateTexture(w,h,levels,0,fmt,D3DPOOL_DEFAULT,&gpu,0)) || !gpu)
        return S_FALSE;
    IDirect3DTexture9* stage=0;
    if (FAILED(real->CreateTexture(w,h,levels,0,fmt,D3DPOOL_SYSTEMMEM,&stage,0)) || !stage) {
        gpu->Release();
        return S_FALSE;
    }
    VaultTexture* v=new(std::nothrow) VaultTexture(gpu,stage,w,h,levels,fmt);
    if (!v || !v->Probe()) {
        delete v;
        stage->Release();
        gpu->Release();
        return S_FALSE;
    }
    v->m_next=g_head;
    if (g_head) g_head->m_prev=v;
    g_head=v;
    ++g_count;
    g_hotBytes+=v->m_bytes;
    *out=v;
    return D3D_OK;
}
HRESULT OCV_SetTexture(IDirect3DDevice9* real, DWORD stage, IDirect3DBaseTexture9* tex) {
    IDirect3DBaseTexture9* underlying=tex;
    VaultTexture* vault=0;
    const int i=StageIndex(stage);
    if (IsVault(tex)) {
        vault=(VaultTexture*)tex;
        if (!vault->m_native && (!vault->m_real || vault->m_notUp)) {
            Guard g;
            vault->Ready();
        }
        underlying=vault->m_real;
        if (!underlying) return D3DERR_INVALIDCALL; // never silently unbind a missing GPU copy
    }
    HRESULT hr=real->SetTexture(stage,underlying);
    if (SUCCEEDED(hr) && real==g_dev && i>=0) {
        // Track only successful bindings, never pin the texture through this
        // observer. The underlying device owns its bound reference.
        g_stageTex[i]=tex;
        g_stageVault[i]=vault;
    }
    return hr;
}
HRESULT OCV_GetTexture(IDirect3DDevice9* real, DWORD stage, IDirect3DBaseTexture9** out) {
    HRESULT hr=real->GetTexture(stage,out);
    const int i=StageIndex(stage);
    if (SUCCEEDED(hr) && out && *out && real==g_dev && i>=0 && g_stageVault[i]) {
        VaultTexture* v=g_stageVault[i];
        if (v->m_real==*out) {
            (*out)->Release();
            v->AddRef();
            *out=v;
        }
    }
    return hr;
}
HRESULT OCV_UpdateTexture(IDirect3DDevice9* real, IDirect3DBaseTexture9* src,
                          IDirect3DBaseTexture9* dst) {
    if (IsVault(src)) src=((VaultTexture*)src)->m_real;
    if (IsVault(dst)) dst=((VaultTexture*)dst)->m_real;
    return real->UpdateTexture(src,dst);
}
OCVStats OCV_ReadStats() {
    OCVStats out={};
    if (!g_initState) return out;
    Guard g;
    out.vaultCount=g_count;
    out.heldMb=(unsigned)(g_coldBytes>>20);
    out.hotMb=(unsigned)(g_hotBytes>>20);
    out.relocks=g_relocks;
    out.natives=g_natives;
    out.failures=g_failures;
    out.usedVaMb=g_usedVaMb;
    out.peakVaMb=g_peakVaMb;
    return out;
}
