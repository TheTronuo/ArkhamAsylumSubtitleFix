#define SUBTITLE_TEST
#include "SubtitleScale.cpp"
#include <cassert>
#include <initializer_list>
#include <dinput.h>

static float observedX,observedY,observedSX,observedSY;
static void* observedCanvas;
static void __cdecl MockMeasure(void*,int* w,int* h,const wchar_t*,...) { *w=200; *h=30; }
static void __cdecl MockDraw(void* canvas,float x,float y,const wchar_t*,void*,const void*,float sx,float sy,float spacing,const void* extra) {
    observedCanvas=canvas; observedX=x; observedY=y; observedSX=sx; observedSY=sy;
    assert(spacing==0 && !extra);
}
static void CheckRejectedMutation(const wchar_t* path) {
    // Change one byte in a temporary copy, leaving the supplied EXE untouched.
    wchar_t temporary[MAX_PATH];
    assert(GetTempFileNameW(L"build",L"ass",0,temporary));
    assert(CopyFileW(path,temporary,FALSE));
    HANDLE file=CreateFileW(temporary,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    assert(file!=INVALID_HANDLE_VALUE);
    BYTE value; DWORD count;
    assert(ReadFile(file,&value,1,&count,nullptr) && count==1);
    value^=1;
    assert(SetFilePointer(file,0,nullptr,FILE_BEGIN)==0);
    assert(WriteFile(file,&value,1,&count,nullptr) && count==1);
    CloseHandle(file);
    bool accepted=SupportedExe(temporary);
    assert(DeleteFileW(temporary));
    assert(!accepted);
}
int wmain(int argc,wchar_t** argv) {
    g_measure=MockMeasure; g_draw=MockDraw; g_drawLogged=1;
    for(float scale:{1.0f,1.5f,2.0f,2.5f,3.0f}) {
        g_scale=scale;
        for(float offset:{-160.0f,0.0f,160.0f}) {
        g_verticalOffset=offset;
        DrawScaled((void*)1,1000,1900,L"A long subtitle",(void*)2,(void*)3);
        assert(observedCanvas==(void*)1 && observedX==1000-100*scale && observedY==1900+offset);
        assert(observedSX==scale && observedSY==scale);
        DrawScaled((void*)1,1001,1901,L"A long subtitle",(void*)2,(void*)3);
        assert(observedX==1001-100*scale && observedY==1901+offset);
        }
    }
    g_steps[0]=36; g_steps[1]=52;
    int a0,a1,c0,c1;
    __asm { xor eax,eax }
    __asm { call StepEax }
    __asm { mov a0,eax }
    __asm { mov eax,1 }
    __asm { call StepEax }
    __asm { mov a1,eax }
    __asm { xor ecx,ecx }
    __asm { call StepEcx }
    __asm { mov c0,ecx }
    __asm { mov ecx,1 }
    __asm { call StepEcx }
    __asm { mov c1,ecx }
    assert(a0==36 && a1==52 && c0==36 && c1==52);
    BYTE* memory=(BYTE*)VirtualAlloc(nullptr,0x960000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(memory);
    Patch patches[kPatchCount]={}; MakePatches(memory,patches);
    for(const auto& p:patches) memcpy(p.at,p.before,p.size);
    patches[12].at[0]^=1;
    assert(!ApplyPatches(patches));
    for(unsigned i=0;i<12;i++) assert(!memcmp(patches[i].at,patches[i].before,patches[i].size));
    patches[12].at[0]^=1;
    DWORD old; assert(VirtualProtect(memory,0x960000,PAGE_EXECUTE_READ,&old));
    assert(ApplyPatches(patches));
    for(const auto& p:patches) {
        assert(!memcmp(p.at,p.after,p.size));
        MEMORY_BASIC_INFORMATION info; assert(VirtualQuery(p.at,&info,sizeof(info)));
        assert(info.Protect==PAGE_EXECUTE_READ);
    }
    VirtualFree(memory,0,MEM_RELEASE);
    if(argc>1) for(int i=1;i<argc;i++) {
        assert(SupportedExe(argv[i]));
        CheckRejectedMutation(argv[i]);
    }
    else puts("SKIP: supported game EXE hash check (supply its absolute path to enable).");
    assert(!SupportedExe(L"build\\Tests.exe"));
    HMODULE proxy=LoadLibraryW(L"build\\dinput8.dll"); assert(proxy);
    using CreateFn=HRESULT (WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
    auto create=(CreateFn)GetProcAddress(proxy,"DirectInput8Create"); assert(create);
    const IID iid={0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
    IUnknown* input=nullptr;
    HRESULT hr=create(GetModuleHandleW(nullptr),0x0800,iid,(void**)&input,nullptr);
    assert(SUCCEEDED(hr) && input); input->Release();
    puts("PASS: centered draw ABI/scales/vertical offsets/shadow alignment, x86 line hooks, conflict refusal, patch transaction/protections, EXE SHA256, DirectInput proxy forwarding.");
    return 0;
}
