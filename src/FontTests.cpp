#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cwchar>
#include "NativeFont.h"
static void Print(const char* s) {fputs(s,stdout);fflush(stdout);}
static HMODULE MapTestDecoder(const wchar_t* exe) {
    // Windows maps an EXE as an image without relocating it or running it.
    // Relocate only the already inspected decoder's switch operand/table in
    // this test process. The game file and production code are never changed.
    HMODULE game=LoadLibraryExW(exe,nullptr,DONT_RESOLVE_DLL_REFERENCES);assert(game);
    auto base=reinterpret_cast<unsigned char*>(game);
    auto code=base+0x13fbf50;constexpr unsigned codeSize=0x53d8,total=codeSize+768*4;
    DWORD protection;assert(VirtualProtect(code,total,PAGE_READWRITE,&protection));
    const unsigned delta=static_cast<unsigned>(reinterpret_cast<uintptr_t>(base))-0x400000;
    unsigned pointer;memcpy(&pointer,code+0xcc,4);pointer+=delta;memcpy(code+0xcc,&pointer,4);
    for(unsigned i=0;i<768;i++) {memcpy(&pointer,code+codeSize+i*4,4);pointer+=delta;memcpy(code+codeSize+i*4,&pointer,4);}
    assert(VirtualProtect(code,total,PAGE_EXECUTE_READ,&protection));FlushInstructionCache(GetCurrentProcess(),code,total);
    return game;
}
int wmain(int argc,wchar_t** argv) {
    SetNativeFontDiagnostics(true);
    assert(TestNativeFontRejection());
    if(argc<2) {puts("PASS: malformed package/movie/glyph rejection. Supply game root and optional localized package for resource tests.");return 0;}
    assert(!PrepareNativeFonts(L"build\\missing-native-font-test-root",Print));
    wchar_t exe[MAX_PATH];swprintf_s(exe,L"%s\\Binaries\\ShippingPC-BmGame.exe",argv[1]);
    for(int i=2;i<argc;i++) if(!wcscmp(argv[i],L"-exe")) {assert(i+1<argc);wcscpy_s(exe,argv[++i]);}
    HMODULE game=MapTestDecoder(exe);
    assert(!InitializeNativeFontDecoder(nullptr,Print));
    assert(InitializeNativeFontDecoder(game,Print));
    // An altered decoder must be rejected before any decompression call.
    auto decoder=reinterpret_cast<unsigned char*>(game)+0x13fbf50;DWORD protection;
    assert(VirtualProtect(decoder,1,PAGE_EXECUTE_READWRITE,&protection));*decoder^=1;
    assert(!InitializeNativeFontDecoder(game,Print));
    assert(!TestNativeFontDecoderBounds());
    *decoder^=1;DWORD ignored;assert(VirtualProtect(decoder,1,protection,&ignored));
    FlushInstructionCache(GetCurrentProcess(),decoder,1);
    assert(InitializeNativeFontDecoder(game,Print));
    assert(TestNativeFontDecoderBounds());
    assert(PrepareNativeFonts(argv[1],Print));
    wchar_t pattern[MAX_PATH];swprintf_s(pattern,L"%s\\BmGame\\CookedPC\\Startup_*.upk",argv[1]);
    CreateDirectoryW(L"build\\font-previews",nullptr);
    WIN32_FIND_DATAW found;HANDLE search=FindFirstFileW(pattern,&found);assert(search!=INVALID_HANDLE_VALUE);unsigned packages=0;
    do {
        if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\BmGame\\CookedPC\\%s",argv[1],found.cFileName);
        assert(TestNativeFontPackage(path,L"build\\font-previews",Print));packages++;
    } while(FindNextFileW(search,&found));FindClose(search);
    for(int i=2;i<argc;i++) {if(!wcscmp(argv[i],L"-exe")) {i++;continue;}assert(TestNativeFontPackage(argv[i],L"build\\font-previews",Print));packages++;}
    assert(TestNativeFontGPU(Print));
    SetNativeFontDiagnostics(false);
    assert(TestNativeFontGPU(Print));
    printf("PASS: %u real localization packages decompressed, original outlines parsed and native font pages generated.\n",packages);
    FreeLibrary(game);
}
