#pragma once
#include <cstddef>
using FontLog = void (*)(const char*);
void SetNativeFontDiagnostics(bool enabled);
void EnterNativeFontDraw();
void LeaveNativeFontDraw();
struct NativeFontDrawScope {
    NativeFontDrawScope() {EnterNativeFontDraw();}
    ~NativeFontDrawScope() {LeaveNativeFontDraw();}
    NativeFontDrawScope(const NativeFontDrawScope&)=delete;
    NativeFontDrawScope& operator=(const NativeFontDrawScope&)=delete;
};
// Only a verified original decoder in an already mapped game EXE is used.
bool InitializeNativeFontDecoder(void* gameModule, FontLog log);
bool TestNativeFontDecoderBounds();
// Extracts original contours from installed packages; never edits game assets.
bool PrepareNativeFonts(const wchar_t* gameRoot, FontLog log);
bool InstallNativeFontHooks(FontLog log);
bool TestNativeFontPackage(const wchar_t* path, const wchar_t* previewDirectory, FontLog log);
bool TestNativeFontRejection();
bool TestNativeFontGPU(FontLog log);
