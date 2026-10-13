#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <cstdint>
#include "NativeFont.h"

static float g_scale = 2.0f;
static float g_verticalOffset = 0.0f;
static float g_cutsceneVerticalOffset = 0.0f;
static int g_steps[2] = {36,52};
using MeasureFn = void (__cdecl*)(void*,int*,int*,const wchar_t*,...);
using DrawFn = void (__cdecl*)(void*,float,float,const wchar_t*,void*,const void*,float,float,float,const void*);
static MeasureFn g_measure;
static DrawFn g_draw;
static wchar_t g_logPath[MAX_PATH];
static LONG g_drawLogged;
static void Log(const char* message) {
    HANDLE f=CreateFileW(g_logPath,FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f!=INVALID_HANDLE_VALUE) { DWORD n; WriteFile(f,message,(DWORD)strlen(message),&n,nullptr); CloseHandle(f); }
}
static void DrawScaledWithOffset(void* canvas,float center,float y,const wchar_t* text,void* font,const void* color,float offset) {
    if(!canvas || !font || !text) return;
    int width=0,height=0;
    // Preserve the original wrapper's varargs measurement ABI.
    g_measure(font,&width,&height,text);
    NativeFontDrawScope nativeFontDraw;
    g_draw(canvas,center-width*g_scale*0.5f,y+offset,text,font,color,g_scale,g_scale,0.0f,nullptr);
    if(InterlockedCompareExchange(&g_drawLogged,1,0)==0) Log("Subtitle drawing hook reached.\r\n");
}
static void __cdecl DrawScaled(void* canvas,float center,float y,const wchar_t* text,void* font,const void* color) {
    DrawScaledWithOffset(canvas,center,y,text,font,color,g_verticalOffset);
}
static void __cdecl DrawCutsceneScaled(void* canvas,float center,float y,const wchar_t* text,void* font,const void* color) {
    DrawScaledWithOffset(canvas,center,y,text,font,color,g_cutsceneVerticalOffset);
}
static bool ReadVerticalOffsets(const wchar_t* ini) {
    wchar_t value[64],cutsceneValue[64];
    GetPrivateProfileStringW(L"Subtitles",L"VerticalOffset",L"0",value,64,ini);
    wchar_t* end=nullptr; double parsed=wcstod(value,&end);
    if(end==value || *end || !std::isfinite(parsed) || parsed<-1000.0 || parsed>1000.0) {
        Log("Invalid VerticalOffset; use -1000 to 1000 pixels. No hooks installed.\r\n"); return false;
    }
    // Existing INIs keep their previous video subtitle position.
    GetPrivateProfileStringW(L"Subtitles",L"CutsceneVerticalOffset",value,cutsceneValue,64,ini);
    double cutsceneParsed=wcstod(cutsceneValue,&end);
    if(end==cutsceneValue || *end || !std::isfinite(cutsceneParsed) || cutsceneParsed<-1000.0 || cutsceneParsed>1000.0) {
        Log("Invalid CutsceneVerticalOffset; use -1000 to 1000 pixels. No hooks installed.\r\n"); return false;
    }
    g_verticalOffset=(float)parsed;
    g_cutsceneVerticalOffset=(float)cutsceneParsed;
    return true;
}
// LEA replacements preserve flags and every register except the destination.
static __declspec(naked) void StepEax() {
    __asm { mov eax,dword ptr [g_steps+eax*4] }
    __asm { ret }
}
static __declspec(naked) void StepEcx() {
    __asm { mov ecx,dword ptr [g_steps+ecx*4] }
    __asm { ret }
}
struct Patch { BYTE* at; BYTE before[8]; BYTE after[8]; SIZE_T size; };
static constexpr unsigned kPatchCount=13;
static void CallBytes(BYTE* out,BYTE* at,const void* target) {
    out[0]=0xe8;
    uint32_t relative=(uint32_t)((uintptr_t)target-(uintptr_t)(at+5));
    memcpy(out+1,&relative,4);
}
static void MakePatches(BYTE* base,Patch (&p)[kPatchCount]) {
    constexpr uint32_t draws[]={0x9458d7,0x945913,0x94594f,0x94598b,0x9459c0,
                               0x956212,0x95624e,0x95628a,0x9562c6,0x9562fb};
    for(unsigned i=0;i<10;i++) {
        p[i].at=base+draws[i]; p[i].size=5;
        CallBytes(p[i].before,p[i].at,base+0x6b00a0);
        // First five calls are gameplay; last five are video text and shadows.
        CallBytes(p[i].after,p[i].at,i<5?DrawScaled:DrawCutsceneScaled);
    }
    // One load supplies both confirmed layout ScaleX/ScaleY fields.
    p[10].at=base+0x955ea5; p[10].size=8;
    const BYTE load[]={0xf3,0x0f,0x10,0x0d,0,0,0,0};
    memcpy(p[10].before,load,8); memcpy(p[10].after,load,8);
    uint32_t original=(uint32_t)(uintptr_t)(base+0x1a3f35c);
    uint32_t replacement=(uint32_t)(uintptr_t)&g_scale;
    memcpy(p[10].before+4,&original,4); memcpy(p[10].after+4,&replacement,4);
    const BYTE a[]={0x8d,0x04,0xc5,0x12,0,0,0}, c[]={0x8d,0x0c,0xcd,0x12,0,0,0};
    p[11].at=base+0x945891; p[11].size=7;
    p[12].at=base+0x95614e; p[12].size=7;
    memcpy(p[11].before,a,7); memcpy(p[12].before,c,7);
    CallBytes(p[11].after,p[11].at,StepEax); CallBytes(p[12].after,p[12].at,StepEcx);
    p[11].after[5]=p[11].after[6]=p[12].after[5]=p[12].after[6]=0x90;
}
static bool ApplyPatches(Patch (&p)[kPatchCount]) {
    // Refuse conflicting bytes before making any change.
    for(const auto& site:p) if(memcmp(site.at,site.before,site.size)) return false;
    DWORD old[kPatchCount]={}; unsigned n=0;
    for(;n<kPatchCount;n++) if(!VirtualProtect(p[n].at,p[n].size,PAGE_EXECUTE_READWRITE,&old[n])) break;
    if(n!=kPatchCount) {
        while(n) { --n; DWORD ignored; VirtualProtect(p[n].at,p[n].size,old[n],&ignored); }
        return false;
    }
    for(const auto& site:p) memcpy(site.at,site.after,site.size);
    FlushInstructionCache(GetCurrentProcess(),nullptr,0);
    // Sites share pages; reverse restore recovers their original protection.
    for(unsigned i=kPatchCount;i-->0;) { DWORD ignored; VirtualProtect(p[i].at,p[i].size,old[i],&ignored); }
    return true;
}
static bool SupportedExe(const wchar_t* path) {
    // Exact inspected builds only. Their 13 subtitle hook sites are identical.
    const BYTE expected[][32]={
        // Original Steam GOTY.
        {0x4d,0xac,0x1f,0x5e,0x2a,0xc6,0x71,0x0b,0x73,0x78,0xfd,0xce,0x74,0x60,0x1f,0x61,
         0x6f,0x47,0x53,0xe3,0x75,0x6c,0xb5,0xfd,0xa6,0x3c,0x75,0x19,0xcc,0x2e,0xb0,0x28},
        // Supplied GOG build, without the Joker DLC patch.
        {0x94,0x83,0xc5,0x08,0x4c,0x7e,0xf0,0x2d,0x72,0x43,0x61,0x6d,0xbc,0x31,0x21,0x93,
         0x63,0x65,0x2f,0xc7,0x3b,0xb2,0x9a,0x15,0x5a,0x99,0x40,0xf5,0xcb,0x1c,0xc6,0xcd},
        // Same supplied build with the two-byte Joker DLC patch.
        {0x6d,0x10,0x55,0xfb,0xd9,0xb9,0x47,0xd6,0xa1,0x73,0x71,0x1b,0xbd,0x73,0x7a,0x11,
         0xb9,0x70,0x76,0xfd,0xaf,0x3b,0x0c,0x7c,0xb6,0xae,0xba,0xd0,0x05,0xd7,0xf5,0xe6}
    };
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider=0; HCRYPTHASH hash=0;
    bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);
    BYTE buffer[65536]; DWORD n;
    while(ok) {
        if(!ReadFile(f,buffer,sizeof(buffer),&n,nullptr)) { ok=false; break; }
        if(!n) break;
        ok=CryptHashData(hash,buffer,n,0)!=FALSE;
    }
    BYTE actual[32]; DWORD size=sizeof(actual);
    ok=ok && CryptGetHashParam(hash,HP_HASHVAL,actual,&size,0) && size==32;
    bool supported=false;
    if(ok) for(const auto& candidate:expected) if(!memcmp(actual,candidate,32)) { supported=true; break; }
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider,0);
    CloseHandle(f); return supported;
}
#ifndef SUBTITLE_TEST
extern "C" __declspec(dllexport) BOOL __cdecl InitializeSubtitleScale() {
    static LONG once=0;
    if(InterlockedCompareExchange(&once,1,0)) return TRUE;
    wchar_t exe[MAX_PATH],ini[MAX_PATH];
    DWORD length=GetModuleFileNameW(nullptr,exe,MAX_PATH);
    if(!length || length>=MAX_PATH) return FALSE;
    wcscpy_s(ini,exe); wchar_t* slash=wcsrchr(ini,L'\\');
    if(!slash) return FALSE;
    wcscpy_s(slash+1,MAX_PATH-(slash+1-ini),L"ArkhamSubtitleScale.ini");
    wcscpy_s(g_logPath,ini); wchar_t* dot=wcsrchr(g_logPath,L'.');
    wcscpy_s(dot,MAX_PATH-(dot-g_logPath),L".log");
    HANDLE log=CreateFileW(g_logPath,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log!=INVALID_HANDLE_VALUE) CloseHandle(log);
    Log("Arkham Subtitle Scale 0.3.0 (x86), original native font contours; game LZO decoder; menu performance fix.\r\n");
    if(!GetPrivateProfileIntW(L"Subtitles",L"Enabled",1,ini)) { Log("Disabled; no hooks installed.\r\n"); return TRUE; }
    wchar_t value[64]; GetPrivateProfileStringW(L"Subtitles",L"Scale",L"2.0",value,64,ini);
    wchar_t* end=nullptr; double parsed=wcstod(value,&end);
    if(end==value || *end || !std::isfinite(parsed) || parsed<0.5 || parsed>4.0) {
        Log("Invalid Scale; use 0.5 to 4.0 with a decimal point. No hooks installed.\r\n"); return FALSE;
    }
    g_scale=(float)parsed;
    if(!ReadVerticalOffsets(ini)) return FALSE;
    g_steps[0]=(int)std::lround(18.0f*g_scale); g_steps[1]=(int)std::lround(26.0f*g_scale);
    if(!SupportedExe(exe)) { Log("Unsupported EXE SHA256; no hooks installed.\r\n"); return FALSE; }
    BYTE* base=(BYTE*)GetModuleHandleW(nullptr);
    auto dos=(IMAGE_DOS_HEADER*)base; auto nt=(IMAGE_NT_HEADERS32*)(base+dos->e_lfanew);
    if(nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 || nt->OptionalHeader.SizeOfImage!=40579072) {
        Log("Unexpected PE layout; no hooks installed.\r\n"); return FALSE;
    }
    wchar_t gameRoot[MAX_PATH];wcscpy_s(gameRoot,exe);
    wchar_t* binarySlash=wcsrchr(gameRoot,L'\\');if(!binarySlash) return FALSE;*binarySlash=0;
    wchar_t* rootSlash=wcsrchr(gameRoot,L'\\');if(!rootSlash) return FALSE;*rootSlash=0;
    if(GetPrivateProfileIntW(L"Subtitles",L"NativeFont",1,ini)) {
        if(!InitializeNativeFontDecoder(base,Log) || !PrepareNativeFonts(gameRoot,Log) || !InstallNativeFontHooks(Log))
            Log("Native fonts unavailable. Falling back to the original bitmap scaling, with the same layout and offsets.\r\n");
    } else Log("NativeFont=0. Using the original bitmap scaling.\r\n");
    g_measure=(MeasureFn)(base+0x6584b0); g_draw=(DrawFn)(base+0x6a7ed0);
    Patch patches[kPatchCount]={}; MakePatches(base,patches);
    if(!ApplyPatches(patches)) { Log("Hook validation/protection failed; no hooks installed.\r\n"); return FALSE; }
    char message[224]; sprintf_s(message,"Installed 13 hooks. Scale=%.3f; VerticalOffset=%.1f px; CutsceneVerticalOffset=%.1f px; line steps=%d/%d.\r\n",g_scale,g_verticalOffset,g_cutsceneVerticalOffset,g_steps[0],g_steps[1]);
    Log(message); return TRUE;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
#endif
