#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include "NativeFont.h"
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cassert>
#include <thread>

namespace {
using Bytes=std::vector<unsigned char>;
constexpr unsigned kResolution=4;
FontLog logger;
void Note(const char* s) { if(logger) logger(s); }
[[noreturn]] void Bad(const char* s) { throw std::runtime_error(s); }
// The inspected Steam/GOG builds contain the same bounded LZO1X decoder.
// Call it in the game image: no decoder code or compressed game assets ship here.
using LzoFn=int (__cdecl*)(const unsigned char*,unsigned,unsigned char*,unsigned*,void*);
LzoFn gameLzo=nullptr;
constexpr unsigned kLzoRva=0x13fbf50, kLzoCodeSize=0x53d8, kLzoTableEntries=768;
bool ResolveGameLzo(void* module) {
    gameLzo=nullptr;
    if(!module) return false;
    auto base=static_cast<unsigned char*>(module);
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(base,&memory,sizeof(memory)) || memory.Type!=MEM_IMAGE || memory.AllocationBase!=base) return false;
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>4096-sizeof(IMAGE_NT_HEADERS32)) return false;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 ||
       nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.SizeOfImage!=40579072) return false;
    auto code=base+kLzoRva;
    constexpr unsigned total=kLzoCodeSize+kLzoTableEntries*4;
    if(!VirtualQuery(code,&memory,sizeof(memory)) || memory.AllocationBase!=base || memory.State!=MEM_COMMIT ||
       (memory.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
       !(memory.Protect&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
       memory.RegionSize<total+size_t(code-static_cast<unsigned char*>(memory.BaseAddress))) return false;
    // Normalize the one absolute jump operand and its switch table for ASLR,
    // then verify every byte before calling the original bounded routine.
    Bytes normalized(code,code+total);
    const unsigned actualBase=static_cast<unsigned>(reinterpret_cast<uintptr_t>(base));
    unsigned jump;memcpy(&jump,normalized.data()+0xcc,4);
    if(jump!=actualBase+kLzoRva+kLzoCodeSize) return false;
    jump-=actualBase;jump+=0x400000;memcpy(normalized.data()+0xcc,&jump,4);
    for(unsigned i=0;i<kLzoTableEntries;i++) {
        unsigned target;memcpy(&target,normalized.data()+kLzoCodeSize+i*4,4);
        if(target<actualBase+kLzoRva || target>=actualBase+kLzoRva+kLzoCodeSize) return false;
        target-=actualBase;target+=0x400000;memcpy(normalized.data()+kLzoCodeSize+i*4,&target,4);
    }
    const unsigned char expected[32]={0x0c,0x26,0x7b,0x39,0x09,0xaa,0xae,0x90,0xbf,0x50,0xe1,0xc0,0x03,0xa5,0xaa,0xdb,
        0x98,0x14,0xab,0x4c,0x05,0xfe,0x6e,0x4f,0x3f,0x93,0x43,0x05,0xf2,0x1d,0xd0,0x54};
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;unsigned char digest[32];DWORD size=sizeof(digest);
    bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider,CALG_SHA_256,0,0,&hash) && CryptHashData(hash,normalized.data(),total,0) &&
        CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0) && size==32 && !memcmp(digest,expected,32);
    if(hash) CryptDestroyHash(hash);if(provider) CryptReleaseContext(provider,0);
    if(!ok) return false;
    gameLzo=reinterpret_cast<LzoFn>(code);return true;
}
bool DecodeLzoBlock(const unsigned char* source,unsigned sourceSize,unsigned char* dest,unsigned destSize,bool* fault=nullptr) {
    if(fault) *fault=false;
    if(!gameLzo || !source || !dest || !sourceSize || !destSize || sourceSize>2*1024*1024 || destSize>1024*1024) return false;
    unsigned written=destSize;
    // Keep structured exception handling in this leaf, without C++ objects.
    __try {return gameLzo(source,sourceSize,dest,&written,nullptr)==0 && written==destSize;}
    __except(EXCEPTION_EXECUTE_HANDLER) {if(fault) *fault=true;return false;}
}
struct Reader {
    const Bytes& b; size_t p;
    Reader(const Bytes& bytes,size_t at=0):b(bytes),p(at) {}
    void check(size_t n) const { if(p>b.size() || n>b.size()-p) Bad("truncated resource"); }
    unsigned u8() { check(1); return b[p++]; }
    uint16_t u16() { check(2); uint16_t v; memcpy(&v,b.data()+p,2); p+=2; return v; }
    uint32_t u32() { check(4); uint32_t v; memcpy(&v,b.data()+p,4); p+=4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    void skip(size_t n) { check(n); p+=n; }
    Bytes bytes(size_t n) { check(n); Bytes v(b.begin()+p,b.begin()+p+n); p+=n; return v; }
    std::string string() {
        int32_t n=i32(); if(n==INT32_MIN || std::abs(n)>65536) Bad("invalid string");
        std::string s;
        if(n>=0) { check((size_t)n); s.assign((const char*)b.data()+p,(size_t)n); p+=(size_t)n; }
        else { for(int i=0;i<-n;i++) { unsigned c=u16(); if(c>127 && c) Bad("non-ASCII package name"); s.push_back((char)c); } }
        if(!s.empty() && s.back()==0) s.pop_back();
        return s;
    }
};
Bytes ReadFileBytes(const wchar_t* path) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) Bad("cannot read package");
    LARGE_INTEGER size; DWORD n=0;
    if(!GetFileSizeEx(file,&size) || size.QuadPart<24 || size.QuadPart>64*1024*1024) { CloseHandle(file); Bad("invalid package length"); }
    Bytes b((size_t)size.QuadPart);
    bool ok=ReadFile(file,b.data(),(DWORD)b.size(),&n,nullptr) && n==b.size();
    CloseHandle(file); if(!ok) Bad("package read failed"); return b;
}
struct Export { int32_t klass,outer; std::string name; unsigned offset,size; };
struct Package {
    Bytes data; std::vector<std::string> names,imports; std::vector<Export> exports;
    explicit Package(const Bytes& raw) {
        Reader h(raw); if(h.u32()!=0x9e2a83c1 || h.u32()!=0x150240) Bad("unsupported UE3 package version");
        h.skip(4); h.string(); size_t p=h.p;
        h.skip(4); unsigned nc=h.u32(),no=h.u32(),ec=h.u32(),eo=h.u32(),ic=h.u32(),io=h.u32();
        if(nc>100000 || ec>100000 || ic>100000) Bad("invalid package tables");
        Reader gen(raw,p+48); unsigned gc=gen.u32(); if(gc>32) Bad("invalid generations");
        Reader comp(raw,p+52+gc*12+8); unsigned method=comp.u32(),cc=comp.u32();
        if(cc>128 || (cc && method!=2)) Bad("unsupported package compression");
        struct Chunk {unsigned off,size,disk,compressed;}; std::vector<Chunk> chunks;
        for(unsigned i=0;i<cc;i++) chunks.push_back({comp.u32(),comp.u32(),comp.u32(),comp.u32()});
        if(chunks.empty()) data=raw;
        else { Reader r(raw); data=r.bytes(chunks[0].off); }
        for(const auto& c:chunks) {
            if(c.size>128*1024*1024 || data.size()!=c.off || c.off+c.size>128*1024*1024) Bad("invalid chunk extent");
            Reader r(raw,c.disk); if(r.u32()!=0x9e2a83c1) Bad("invalid chunk signature");
            unsigned bs=r.u32(); r.u32(); unsigned total=r.u32();
            if(!bs || bs>1024*1024 || total!=c.size) Bad("invalid block size");
            unsigned count=(total+bs-1)/bs;
            std::vector<std::pair<unsigned,unsigned>> blocks;
            for(unsigned i=0;i<count;i++) { unsigned compressed=r.u32(),uncompressed=r.u32(); blocks.push_back({compressed,uncompressed}); }
            for(const auto& block:blocks) {
                if(block.second>bs) Bad("invalid LZO block extent");
                r.check(block.first); size_t before=data.size(); data.resize(before+block.second);
                if(!DecodeLzoBlock(raw.data()+r.p,block.first,data.data()+before,block.second)) Bad("game LZO decompression failed");
                r.skip(block.first);
            }
            if(data.size()!=c.off+c.size) Bad("chunk length mismatch");
        }
        Reader nr(data,no);
        for(unsigned i=0;i<nc;i++) { names.push_back(nr.string()); nr.skip(8); }
        for(unsigned i=0;i<ic;i++) { Reader r(data,io+i*28); r.skip(20); imports.push_back(name(r.u32())); r.skip(4); }
        for(unsigned i=0;i<ec;i++) {
            Reader r(data,eo+i*68); int32_t cl=r.i32(); r.i32(); int32_t outer=r.i32(); std::string nm=name(r.u32()); r.skip(16);
            unsigned sz=r.u32(),off=r.u32(); Reader check(data,off); check.check(sz);
            exports.push_back({cl,outer,nm,off,sz});
        }
    }
    const std::string& name(unsigned n) const { if(n>=names.size()) Bad("bad FName"); return names[n]; }
    std::string ref(int32_t index) const {
        if(index>0 && (size_t)index<=exports.size()) return exports[index-1].name;
        if(index<0 && index!=INT32_MIN && (size_t)-index<=imports.size()) return imports[-index-1];
        if(index==0) return "None"; Bad("bad object reference");
    }
};
struct Property {std::string name,type; Bytes data; bool boolean=false;};
std::vector<Property> Properties(const Package& pkg,size_t off,size_t& end) {
    Reader r(pkg.data,off); r.skip(4); std::vector<Property> props;
    for(unsigned i=0;i<100;i++) {
        std::string nm=pkg.name(r.u32()); r.u32();
        if(nm=="None") { end=r.p; return props; }
        std::string type=pkg.name(r.u32()); r.u32(); unsigned sz=r.u32(); r.u32();
        if(type=="StructProperty") r.skip(8);
        bool value=type=="BoolProperty"?r.u32()!=0:false;
        props.push_back({nm,type,r.bytes(sz),value});
    }
    Bad("unterminated properties");
}
const Bytes& Prop(const std::vector<Property>& p,const char* name) {
    for(const auto& x:p) if(x.name==name) return x.data;
    Bad("missing font property");
}
struct Bits {
    const Bytes& data; size_t p=0;
    explicit Bits(const Bytes& d):data(d) {}
    unsigned u(unsigned n) {
        if(n>31 || p+n>data.size()*8) Bad("truncated glyph bits");
        unsigned v=0; while(n--) {v=(v<<1)|((data[p/8]>>(7-p%8))&1); p++;} return v;
    }
    int s(unsigned n) { unsigned v=u(n); return n && (v&(1u<<(n-1))) ? (int)(v-(1u<<n)):(int)v; }
};
struct Command {char kind; float x,y,cx=0,cy=0;};
struct Glyph {std::vector<Command> commands;};
using Glyphs=std::map<unsigned,Glyph>;
Glyph Shape(const Bytes& bytes) {
    Bits b(bytes); unsigned nf=b.u(4),nl=b.u(4); int x=0,y=0; Glyph g;
    for(unsigned records=0;records<100000;records++) {
        if(!b.u(1)) {
            unsigned flags=b.u(5); if(!flags) return g;
            if(flags&1) {unsigned n=b.u(5); x=b.s(n); y=b.s(n); g.commands.push_back({'M',(float)x,(float)y});}
            if(flags&2) b.u(nf); if(flags&4) b.u(nf); if(flags&8) b.u(nl);
            if(flags&16) Bad("unexpected font styles");
        } else {
            bool straight=b.u(1)!=0; unsigned n=b.u(4)+2;
            if(straight) { if(b.u(1)) {x+=b.s(n);y+=b.s(n);} else if(b.u(1)) y+=b.s(n); else x+=b.s(n); g.commands.push_back({'L',(float)x,(float)y}); }
            else {int cx=x+b.s(n),cy=y+b.s(n); x=cx+b.s(n); y=cy+b.s(n); g.commands.push_back({'Q',(float)x,(float)y,(float)cx,(float)cy});}
        }
    }
    Bad("too many glyph edges");
}
Glyphs DecodeVectorFont(const Bytes& movie) {
    Reader r(movie); auto sig=r.bytes(3);
    if(sig!=Bytes({'G','F','X'}) && sig!=Bytes({'F','W','S'})) Bad("unsupported font movie encoding");
    r.u8(); unsigned length=r.u32(); if(length>movie.size() || length<12) Bad("invalid font movie length");
    unsigned nb=movie[8]>>3; r.p=8+(5+4*nb+7)/8+4;
    while(r.p+2<=length) {
        unsigned word=r.u16(),tag=word>>6,size=word&63;
        if(size==63) size=r.u32(); if(size>length-r.p) Bad("bad movie tag");
        Bytes payload=r.bytes(size);
        if(tag==0) break;
        if(tag!=75) continue;
        Reader f(payload); f.u16(); unsigned flags=f.u8(); f.u8(); unsigned ns=f.u8(); Bytes nm=f.bytes(ns);
        std::string fontName((const char*)nm.data(),nm.size());
        if(fontName!="Rockwell WGL\0" && fontName!="Rockwell WGL") {
            if(!fontName.empty() && fontName.back()==0) fontName.pop_back();
            if(fontName!="Rockwell WGL") continue;
        }
        unsigned count=f.u16(); if(count>8192 || !count) Bad("invalid glyph count");
        size_t base=f.p; std::vector<unsigned> offsets;
        for(unsigned i=0;i<=count;i++) offsets.push_back(flags&8?f.u32():f.u16());
        Reader codes(payload,base+offsets.back()); Glyphs result;
        for(unsigned i=0;i<count;i++) {
            unsigned cp=flags&4?codes.u16():codes.u8();
            if(offsets[i]>offsets[i+1]) Bad("invalid glyph offsets");
            Reader shape(payload,base+offsets[i]); result.emplace(cp,Shape(shape.bytes(offsets[i+1]-offsets[i])));
        }
        return result;
    }
    Bad("original Rockwell vector font was not found");
}
std::unique_ptr<Gdiplus::GraphicsPath> PathFor(const Glyph& glyph) {
    using namespace Gdiplus;
    auto path=std::make_unique<GraphicsPath>(FillModeAlternate); float x=0,y=0; bool figure=false;
    for(const auto& c:glyph.commands) {
        if(c.kind=='M') {if(figure) path->CloseFigure();path->StartFigure();x=c.x;y=c.y;figure=true;}
        else if(c.kind=='L') {path->AddLine(x,y,c.x,c.y);x=c.x;y=c.y;}
        else {path->AddBezier(x,y,x+(c.cx-x)*2/3,y+(c.cy-y)*2/3,c.x+(c.cx-c.x)*2/3,c.y+(c.cy-c.y)*2/3,c.x,c.y);x=c.x;y=c.y;}
    }
    if(figure) path->CloseFigure();
    return path;
}
struct Cell {int x,y,w,h; unsigned page; int vertical; unsigned cp=0;};
struct Atlas {unsigned width=0,height=0; Bytes source; std::vector<uint32_t> original,pixels; unsigned vectorCells=0,retainedNonTextCells=0;};
std::vector<std::shared_ptr<Atlas>> atlases;
void DecodeDxt5(Atlas& a) {
    if(a.source.size()!=((a.width+3)/4)*((a.height+3)/4)*16) Bad("invalid DXT5 length");
    a.original.resize((size_t)a.width*a.height); size_t at=0;
    for(unsigned by=0;by<a.height;by+=4) for(unsigned bx=0;bx<a.width;bx+=4) {
        const unsigned char* p=a.source.data()+at; at+=16; unsigned alpha[8]={p[0],p[1]};
        if(alpha[0]>alpha[1]) for(unsigned i=1;i<=6;i++) alpha[i+1]=((7-i)*alpha[0]+i*alpha[1])/7;
        else {for(unsigned i=1;i<=4;i++) alpha[i+1]=((5-i)*alpha[0]+i*alpha[1])/5;alpha[6]=0;alpha[7]=255;}
        uint64_t ab=0; for(unsigned i=0;i<6;i++) ab|=(uint64_t)p[2+i]<<(8*i);
        uint16_t c0,c1; uint32_t cb; memcpy(&c0,p+8,2);memcpy(&c1,p+10,2);memcpy(&cb,p+12,4);
        unsigned rgb[4][3];
        auto color=[](unsigned c,unsigned* out) {out[0]=((c>>11)&31)*255/31;out[1]=((c>>5)&63)*255/63;out[2]=(c&31)*255/31;};
        color(c0,rgb[0]);color(c1,rgb[1]);
        for(unsigned j=0;j<3;j++) {rgb[2][j]=(2*rgb[0][j]+rgb[1][j])/3;rgb[3][j]=(rgb[0][j]+2*rgb[1][j])/3;}
        for(unsigned i=0;i<16;i++) {unsigned x=bx+i%4,y=by+i/4; if(x>=a.width || y>=a.height) continue; unsigned ci=(cb>>(i*2))&3,al=alpha[(ab>>(i*3))&7];a.original[(size_t)y*a.width+x]=(al<<24)|(rgb[ci][0]<<16)|(rgb[ci][1]<<8)|rgb[ci][2];}
    }
}
Atlas Texture(const Package& pkg,const Export& e) {
    size_t end; auto props=Properties(pkg,e.offset,end); Atlas a;
    Reader x(Prop(props,"SizeX")),y(Prop(props,"SizeY")),fmt(Prop(props,"Format"));
    a.width=x.u32();a.height=y.u32();
    if(pkg.name(fmt.u32())!="PF_DXT5" || !a.width || !a.height || a.width>1024 || a.height>1024) Bad("unsupported font texture");
    Reader r(pkg.data,end);r.skip(16);if(r.u32()!=1) Bad("unexpected font mip count");
    unsigned flags=r.u32(),elements=r.u32(),size=r.u32();r.u32();
    if(flags!=0 || elements!=size || size>a.width*a.height*4) Bad("unsupported font bulk data");
    a.source=r.bytes(size);DecodeDxt5(a);return a;
}
void RenderAtlas(Atlas& atlas,const std::vector<Cell>& cells,unsigned page,const Glyphs& glyphs,bool shadow) {
    using namespace Gdiplus;
    unsigned w=atlas.width*kResolution,h=atlas.height*kResolution;
    atlas.pixels.resize((size_t)w*h);
    // Preserve the game's non-text/control cells; normal letters are reconstructed.
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) atlas.pixels[(size_t)y*w+x]=atlas.original[(size_t)(y/kResolution)*atlas.width+x/kResolution];
    Bitmap image((INT)w,(INT)h,(INT)w*4,PixelFormat32bppARGB,(BYTE*)atlas.pixels.data());
    if(image.GetLastStatus()!=Ok) Bad("native atlas bitmap creation failed");
    Graphics graphics(&image);
    if(graphics.GetLastStatus()!=Ok || graphics.SetSmoothingMode(SmoothingModeAntiAlias)!=Ok || graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality)!=Ok) Bad("native atlas renderer initialization failed");
    SolidBrush white(Color(255,255,255,255)),black(Color(255,0,0,0)),clear(Color(0,255,255,255));
    for(const auto& cell:cells) {
        if(cell.page!=page || !cell.w || !cell.h) continue;
        if(cell.x<0 || cell.y<0 || cell.w<0 || cell.h<0 || cell.x+cell.w>(int)atlas.width || cell.y+cell.h>(int)atlas.height) Bad("invalid glyph rectangle");
        auto it=glyphs.find(cell.cp);
        if(it==glyphs.end() || it->second.commands.empty()) {
            // Tabs, NBSP, game-specific control cells have no font outline.
            // An actual letter without contours is an error, never a RUS placeholder.
            if((cell.cp>=0x21 && cell.cp<=0x7e) || (cell.cp>=0xa1 && cell.cp<=0x24f) || (cell.cp>=0x400 && cell.cp<=0x4ff)) Bad("letter lacks original contours");
            atlas.retainedNonTextCells++; continue;
        }
        int left=cell.w,top=cell.h,right=-1,bottom=-1;
        for(int y=0;y<cell.h;y++) for(int x=0;x<cell.w;x++) {
            uint32_t px=atlas.original[(size_t)(cell.y+y)*atlas.width+cell.x+x];
            if((px>>24)>=128 && ((px>>16)&255)>=128) {left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);}
        }
        if(right<left || bottom<top) Bad("original glyph coverage is empty");
        auto path=PathFor(it->second); RectF bounds; if(path->GetBounds(&bounds)!=Ok || bounds.Width<=0 || bounds.Height<=0) Bad("invalid vector bounds");
        // Align original outline with the original white ink rectangle, retaining
        // the game's character cells, UVs, baseline, spacing and native drop shadow.
        float sx=(right-left+1)*kResolution/bounds.Width,sy=(bottom-top+1)*kResolution/bounds.Height;
        Matrix transform(sx,0,0,sy,(cell.x+left)*kResolution-bounds.X*sx,(cell.y+top)*kResolution-bounds.Y*sy);
        if(path->Transform(&transform)!=Ok) Bad("glyph transform failed");
        Rect clip(cell.x*kResolution,cell.y*kResolution,cell.w*kResolution,cell.h*kResolution);
        if(graphics.SetClip(clip)!=Ok || graphics.SetCompositingMode(CompositingModeSourceCopy)!=Ok || graphics.FillRectangle(&clear,clip)!=Ok || graphics.SetCompositingMode(CompositingModeSourceOver)!=Ok) Bad("glyph cell preparation failed");
        if(shadow) {std::unique_ptr<GraphicsPath> dropped(path->Clone());if(!dropped) Bad("shadow allocation failed");Matrix shift;shift.Translate((REAL)kResolution,(REAL)kResolution);if(dropped->Transform(&shift)!=Ok || graphics.FillPath(&black,dropped.get())!=Ok) Bad("native shadow rendering failed");}
        if(graphics.FillPath(&white,path.get())!=Ok || graphics.ResetClip()!=Ok) Bad("native glyph rendering failed");atlas.vectorCells++;
    }
}
struct GdiSession {
    ULONG_PTR token=0;
    GdiSession() {Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok) Bad("GDI+ initialization failed");}
    ~GdiSession() {if(token) Gdiplus::GdiplusShutdown(token);}
};
void SavePreview(const Atlas& a,const wchar_t* path) {
    UINT count=0,size=0;Gdiplus::GetImageEncodersSize(&count,&size);Bytes storage(size);auto enc=(Gdiplus::ImageCodecInfo*)storage.data();
    if(Gdiplus::GetImageEncoders(count,size,enc)!=Gdiplus::Ok) Bad("cannot enumerate PNG encoder");
    for(UINT i=0;i<count;i++) if(!wcscmp(enc[i].MimeType,L"image/png")) {
        Gdiplus::Bitmap image((INT)a.width*kResolution,(INT)a.height*kResolution,(INT)a.width*kResolution*4,PixelFormat32bppARGB,(BYTE*)a.pixels.data());
        if(image.Save(path,&enc[i].Clsid,nullptr)!=Gdiplus::Ok) Bad("cannot save preview");return;
    }
    Bad("PNG encoder missing");
}
unsigned LoadPackage(const wchar_t* path,const wchar_t* previews) {
    Package pkg(ReadFileBytes(path));Glyphs glyphs;
    for(const auto& e:pkg.exports) if(pkg.ref(e.klass)=="GFxMovieInfo" && e.name=="fonts_en") {
        Reader blob(pkg.data,e.offset);Bytes bytes=blob.bytes(e.size);
        for(size_t i=0;i+8<bytes.size();i++) if(!memcmp(bytes.data()+i,"GFX",3) || !memcmp(bytes.data()+i,"FWS",3)) {glyphs=DecodeVectorFont(Bytes(bytes.begin()+i,bytes.end()));break;}
        break;
    }
    if(glyphs.empty()) Bad("font library absent");
    unsigned added=0;
    for(const auto& e:pkg.exports) if(pkg.ref(e.klass)=="Font" && (e.name=="SmallFont" || e.name=="MediumFont")) {
        size_t end;auto props=Properties(pkg,e.offset,end);
        Reader cr(Prop(props,"Characters"));unsigned count=cr.u32();if(!count || count>8192) Bad("invalid native glyph count");
        std::vector<Cell> cells;
        for(unsigned i=0;i<count;i++) {Cell c;c.x=cr.i32();c.y=cr.i32();c.w=cr.i32();c.h=cr.i32();c.page=cr.u8();c.vertical=cr.i32();cells.push_back(c);}
        Reader remap(pkg.data,end);unsigned rc=remap.u32();if(rc>8192) Bad("invalid character remap");
        for(unsigned i=0;i<rc;i++) {unsigned cp=remap.u16(),index=remap.u16();if(index>=cells.size()) Bad("bad character remap index");cells[index].cp=cp;}
        bool shadow=false;
        Reader options(Prop(props,"ImportOptions"));
        for(unsigned i=0;i<100;i++) {
            std::string nm=pkg.name(options.u32());options.u32();if(nm=="None") break;
            std::string type=pkg.name(options.u32());options.u32();unsigned size=options.u32();options.u32();
            if(type=="StructProperty") options.skip(8);
            if(type=="BoolProperty") {bool value=options.u32()!=0;if(nm=="bEnableDropShadow") shadow=value;}
            options.skip(size);
        }
        Reader tr(Prop(props,"Textures"));unsigned pages=tr.u32();if(!pages || pages>16) Bad("invalid texture page count");
        for(unsigned page=0;page<pages;page++) {
            unsigned index=tr.u32();if(!index || index>pkg.exports.size()) Bad("bad texture object");
            Atlas a=Texture(pkg,pkg.exports[index-1]);
            auto duplicate=std::find_if(atlases.begin(),atlases.end(),[&](const auto& old){return old->width==a.width && old->height==a.height && old->source==a.source;});
            if(duplicate!=atlases.end() && !previews) continue;
            RenderAtlas(a,cells,page,glyphs,shadow);
            if(previews) {wchar_t file[MAX_PATH];swprintf_s(file,L"%s\\%s-%S-%u.png",previews,wcsrchr(path,L'\\')?wcsrchr(path,L'\\')+1:path,e.name.c_str(),page);SavePreview(a,file);}
            char message[200];sprintf_s(message,"Native font atlas: %s page %u, %ux%u -> %ux%u, %u vector cells, %u non-text cells preserved.\r\n",e.name.c_str(),page,a.width,a.height,a.width*kResolution,a.height*kResolution,a.vectorCells,a.retainedNonTextCells);Note(message);
            if(duplicate==atlases.end()) {atlases.push_back(std::make_shared<Atlas>(std::move(a)));added++;}
        }
    }
    return added;
}
} // namespace

bool InitializeNativeFontDecoder(void* gameModule,FontLog log) {
    logger=log;
    try {
        if(ResolveGameLzo(gameModule)) {Note("Verified original game LZO decoder. No bundled decompression library.\r\n");return true;}
    } catch(const std::exception&) {gameLzo=nullptr;}
    Note("Game LZO decoder validation failed. Bitmap scaling remains available.\r\n");return false;
}

bool TestNativeFontDecoderBounds() {
    if(!gameLzo) return false;
    SYSTEM_INFO system;GetSystemInfo(&system);unsigned page=system.dwPageSize;
    struct Guarded {
        unsigned char* allocation;unsigned char* end;
        explicit Guarded(unsigned p):allocation(static_cast<unsigned char*>(VirtualAlloc(nullptr,p*3,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS))),end(nullptr) {
            DWORD protection;
            if(allocation && VirtualProtect(allocation+p,p,PAGE_READWRITE,&protection)) end=allocation+p*2;
        }
        ~Guarded() {if(allocation) VirtualFree(allocation,0,MEM_RELEASE);}
    } input(page),output(page);
    if(!input.end || !output.end) return false;
    const unsigned char literal[]={18,'A',17,0,0},twoLetters[]={19,'A','B',17,0,0};
    bool fault=false;
    memcpy(input.end-sizeof(literal),literal,sizeof(literal));
    memset(output.end-64,0xa5,64);
    if(!DecodeLzoBlock(input.end-sizeof(literal),sizeof(literal),output.end-1,1,&fault) || fault || output.end[-1]!='A') return false;
    for(unsigned n=1;n<sizeof(literal);n++) {
        memcpy(input.end-n,literal,n);
        if(DecodeLzoBlock(input.end-n,n,output.end-1,1,&fault) || fault) return false;
    }
    memcpy(input.end-sizeof(twoLetters),twoLetters,sizeof(twoLetters));
    if(DecodeLzoBlock(input.end-sizeof(twoLetters),sizeof(twoLetters),output.end-1,1,&fault) || fault) return false;
    for(unsigned i=2;i<=64;i++) if(output.end[-static_cast<int>(i)]!=0xa5) return false;
    unsigned random=0x41524b48;
    for(unsigned trial=0;trial<4096;trial++) {
        unsigned length=trial%32+1;
        for(unsigned i=0;i<length;i++) {random=random*1664525+1013904223;(input.end-length)[i]=static_cast<unsigned char>(random>>24);}
        memset(output.end-64,0xa5,64);
        DecodeLzoBlock(input.end-length,length,output.end-32,32,&fault);
        if(fault) return false;
        for(unsigned i=33;i<=64;i++) if(output.end[-static_cast<int>(i)]!=0xa5) return false;
    }
    Note("PASS: original decoder rejects truncated/oversized streams; 4096 malformed inputs stayed within guarded buffers.\r\n");return true;
}

bool PrepareNativeFonts(const wchar_t* root,FontLog log) {
    logger=log;
    try {
        GdiSession gdi;wchar_t pattern[MAX_PATH];swprintf_s(pattern,L"%s\\BmGame\\CookedPC\\Startup_*.upk",root);
        WIN32_FIND_DATAW found;HANDLE search=FindFirstFileW(pattern,&found);if(search==INVALID_HANDLE_VALUE) Bad("startup packages not found");
        std::vector<std::wstring> paths;
        do {if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) {wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\BmGame\\CookedPC\\%s",root,found.cFileName);paths.emplace_back(path);}} while(FindNextFileW(search,&found));FindClose(search);
        for(const auto& path:paths) {
            try {LoadPackage(path.c_str(),nullptr);}
            catch(const std::exception& e) {char msg[256];sprintf_s(msg,"Native font resource rejected: %ls (%s).\r\n",wcsrchr(path.c_str(),L'\\')+1,e.what());Note(msg);atlases.clear();return false;}
        }
        if(atlases.empty()) Bad("no native font atlases built");
        char msg[120];sprintf_s(msg,"Prepared %u unique native font atlases from %u installed localization packages.\r\n",(unsigned)atlases.size(),(unsigned)paths.size());Note(msg);return true;
    } catch(const std::exception& e) {char msg[200];sprintf_s(msg,"Native fonts unavailable: %s. Bitmap scaling remains available.\r\n",e.what());Note(msg);atlases.clear();return false;}
}
bool TestNativeFontPackage(const wchar_t* path,const wchar_t* preview,FontLog log) {
    logger=log;try {GdiSession gdi;LoadPackage(path,preview);return true;}
    catch(const std::exception& e) {Note(e.what());return false;}
}
bool TestNativeFontRejection() {
    try {Package bad(Bytes(40));return false;}catch(const std::exception&) {}
    try {DecodeVectorFont(Bytes(32));return false;}catch(const std::exception&) {}
    try {Shape(Bytes(1,0xff));return false;}catch(const std::exception&) {}
    return true;
}

namespace {
std::recursive_mutex hookMutex;
using CreateDeviceFn=HRESULT (WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
using CreateTextureFn=HRESULT (WINAPI*)(IDirect3DDevice9*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*);
using SetTextureFn=HRESULT (WINAPI*)(IDirect3DDevice9*,DWORD,IDirect3DBaseTexture9*);
using UpdateTextureFn=HRESULT (WINAPI*)(IDirect3DDevice9*,IDirect3DBaseTexture9*,IDirect3DBaseTexture9*);
using UpdateSurfaceFn=HRESULT (WINAPI*)(IDirect3DDevice9*,IDirect3DSurface9*,const RECT*,IDirect3DSurface9*,const POINT*);
using LockTextureFn=HRESULT (WINAPI*)(IDirect3DTexture9*,UINT,D3DLOCKED_RECT*,const RECT*,DWORD);
using UnlockTextureFn=HRESULT (WINAPI*)(IDirect3DTexture9*,UINT);
using ReleaseTextureFn=ULONG (WINAPI*)(IDirect3DTexture9*);
struct DeviceFunctions {CreateTextureFn create;SetTextureFn set;UpdateTextureFn update;UpdateSurfaceFn surface;};
struct TextureFunctions {LockTextureFn lock;UnlockTextureFn unlock;ReleaseTextureFn release;};
using TexturePtr=std::shared_ptr<IDirect3DTexture9>;
struct TextureState {
    unsigned width=0,height=0;D3DLOCKED_RECT locked{};
    bool full=false,replacementFailed=false;uint64_t revision=0;
    std::shared_ptr<Atlas> atlas;TexturePtr replacement;
};
using TextureStatePtr=std::shared_ptr<TextureState>;
thread_local unsigned nativeDrawDepth=0;
LONG lateInspections=0;
std::map<void**,CreateDeviceFn> factoryFunctions;
std::map<void**,DeviceFunctions> deviceFunctions;
std::map<void**,TextureFunctions> textureFunctions;
// Resource Release erases entries during normal operation. Keep the registry
// alive until process exit: running D3D Release from a map's static destructor
// could re-enter that half-destroyed registry while the DLL is shutting down.
std::map<IDirect3DTexture9*,TextureStatePtr>& textures=*new std::map<IDirect3DTexture9*,TextureStatePtr>;
LONG bindLogged=0;
void** Table(void* object) {return *reinterpret_cast<void***>(object);}
bool WriteSlot(void** slot,void* value) {
    DWORD old;if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return false;
    InterlockedExchangePointer(slot,value);DWORD ignored;VirtualProtect(slot,sizeof(void*),old,&ignored);return true;
}
// Never hold our metadata mutex while entering the Direct3D runtime. Its own
// locks may call back into Release on another thread.
template<class Functions> bool OriginalFunctions(const std::map<void**,Functions>& entries,void* object,Functions& out) {
    std::lock_guard<std::recursive_mutex> guard(hookMutex);
    auto it=entries.find(Table(object));if(it==entries.end()) return false;out=it->second;return true;
}
TextureStatePtr FindTexture(IDirect3DTexture9* texture) {
    std::lock_guard<std::recursive_mutex> guard(hookMutex);
    auto it=textures.find(texture);return it==textures.end()?TextureStatePtr{}:it->second;
}
HRESULT WINAPI HookLock(IDirect3DTexture9* texture,UINT level,D3DLOCKED_RECT* rect,const RECT* area,DWORD flags) {
    TextureFunctions functions{};if(!OriginalFunctions(textureFunctions,texture,functions)) return D3DERR_INVALIDCALL;
    HRESULT hr=functions.lock(texture,level,rect,area,flags);
    auto state=FindTexture(texture);TexturePtr retired;
    if(SUCCEEDED(hr) && level==0 && rect && state) {
        std::lock_guard<std::recursive_mutex> guard(hookMutex);
        state->locked=*rect;state->revision++;
        state->full=!area || (area->left==0 && area->top==0 && area->right==(LONG)state->width && area->bottom==(LONG)state->height);
        if(!(flags&D3DLOCK_READONLY)) {
            retired.swap(state->replacement);state->atlas.reset();state->replacementFailed=false;
        }
    }
    return hr;
}
HRESULT WINAPI HookUnlock(IDirect3DTexture9* texture,UINT level) {
    TextureFunctions functions{};if(!OriginalFunctions(textureFunctions,texture,functions)) return D3DERR_INVALIDCALL;
    auto state=FindTexture(texture);
    if(level==0 && state) {
        D3DLOCKED_RECT locked{};bool full;uint64_t revision;
        {
            std::lock_guard<std::recursive_mutex> guard(hookMutex);
            locked=state->locked;full=state->full;revision=state->revision;state->locked={};state->full=false;
        }
        unsigned row=((state->width+3)/4)*16,rows=(state->height+3)/4;
        std::shared_ptr<Atlas> matched;
        if(full && locked.pBits && locked.Pitch>=(INT)row) {
            for(const auto& a:atlases) {
                if(a->width!=state->width || a->height!=state->height) continue;
                bool equal=true;
                for(unsigned y=0;y<rows;y++) if(memcmp((BYTE*)locked.pBits+(size_t)y*locked.Pitch,a->source.data()+(size_t)y*row,row)) {equal=false;break;}
                if(equal) {matched=a;break;}
            }
        }
        bool newlyMatched=false;
        if(matched) {
            std::lock_guard<std::recursive_mutex> guard(hookMutex);
            if(state->revision==revision) {newlyMatched=state->atlas!=matched;state->atlas=matched;}
        }
        if(newlyMatched) Note("Matched original font texture upload.\r\n");
    }
    return functions.unlock(texture,level);
}
ULONG WINAPI HookRelease(IDirect3DTexture9* texture) {
    TextureFunctions functions{};if(!OriginalFunctions(textureFunctions,texture,functions)) return 0;
    ULONG remaining=functions.release(texture);TextureStatePtr retired;
    if(!remaining) {
        std::lock_guard<std::recursive_mutex> guard(hookMutex);
        auto it=textures.find(texture);
        if(it!=textures.end()) {retired=it->second;textures.erase(it);}
    }
    // The last replacement Release runs after the metadata lock is dropped.
    return remaining;
}
bool HookTextureTable(IDirect3DTexture9* texture) {
    std::lock_guard<std::recursive_mutex> guard(hookMutex);
    void** table=Table(texture);if(textureFunctions.count(table)) return true;
    TextureFunctions functions{(LockTextureFn)table[19],(UnlockTextureFn)table[20],(ReleaseTextureFn)table[2]};
    textureFunctions.emplace(table,functions);
    if(WriteSlot(table+19,(void*)HookLock) && WriteSlot(table+20,(void*)HookUnlock) && WriteSlot(table+2,(void*)HookRelease)) return true;
    WriteSlot(table+19,(void*)functions.lock);WriteSlot(table+20,(void*)functions.unlock);WriteSlot(table+2,(void*)functions.release);textureFunctions.erase(table);return false;
}
void TrackTexture(IDirect3DTexture9* texture,UINT width,UINT height,D3DFORMAT format) {
    if(format!=D3DFMT_DXT5) return;
    if(std::none_of(atlases.begin(),atlases.end(),[&](const auto& a){return a->width==width && a->height==height;})) return;
    if(HookTextureTable(texture)) {
        auto state=std::make_shared<TextureState>();state->width=width;state->height=height;
        std::lock_guard<std::recursive_mutex> guard(hookMutex);textures.emplace(texture,state);
    }
}
HRESULT WINAPI HookCreateTexture(IDirect3DDevice9* device,UINT width,UINT height,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** out,HANDLE* shared) {
    DeviceFunctions functions{};if(!OriginalFunctions(deviceFunctions,device,functions)) return D3DERR_INVALIDCALL;
    HRESULT hr=functions.create(device,width,height,levels,usage,format,pool,out,shared);
    if(SUCCEEDED(hr) && out && *out) TrackTexture(*out,width,height,format);
    return hr;
}
TexturePtr CreateReplacement(IDirect3DDevice9* device,const Atlas& a,const DeviceFunctions& functions) {
    unsigned width=a.width*kResolution,height=a.height*kResolution;
    IDirect3DTexture9* raw=nullptr;
    HRESULT hr=functions.create(device,width,height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&raw,nullptr);
    TexturePtr replacement(raw,[](IDirect3DTexture9* p){if(p) p->Release();});
    if(FAILED(hr) || !raw) {Note("Native font GPU texture creation failed; using bitmap scaling for this texture.\r\n");return {};}
    D3DLOCKED_RECT rect{};hr=raw->LockRect(0,&rect,nullptr,0);
    if(FAILED(hr) || !rect.pBits || rect.Pitch<(INT)width*4) {if(SUCCEEDED(hr)) raw->UnlockRect(0);Note("Native font GPU upload failed; using bitmap scaling for this texture.\r\n");return {};}
    for(unsigned y=0;y<height;y++) memcpy((BYTE*)rect.pBits+(size_t)y*rect.Pitch,a.pixels.data()+(size_t)y*width,(size_t)width*4);
    if(FAILED(raw->UnlockRect(0))) return {};
    return replacement;
}
void TransferAtlas(IDirect3DTexture9* source,IDirect3DTexture9* destination) {
    auto src=FindTexture(source),dst=FindTexture(destination);
    if(!dst) {
        D3DSURFACE_DESC desc{};
        if(SUCCEEDED(destination->GetLevelDesc(0,&desc))) {TrackTexture(destination,desc.Width,desc.Height,desc.Format);dst=FindTexture(destination);}
    }
    if(!dst) return;
    TexturePtr retired;bool matched=false;
    {
        std::lock_guard<std::recursive_mutex> guard(hookMutex);
        auto candidate=src && src->width==dst->width && src->height==dst->height?src->atlas:std::shared_ptr<Atlas>{};
        retired.swap(dst->replacement);dst->atlas=candidate;dst->replacementFailed=false;dst->revision++;matched=bool(candidate);
    }
    if(matched) Note("Matched staged font texture transfer.\r\n");
}
HRESULT WINAPI HookUpdateTexture(IDirect3DDevice9* device,IDirect3DBaseTexture9* source,IDirect3DBaseTexture9* destination) {
    DeviceFunctions functions{};if(!OriginalFunctions(deviceFunctions,device,functions)) return D3DERR_INVALIDCALL;
    HRESULT hr=functions.update(device,source,destination);
    if(SUCCEEDED(hr) && source && destination && source->GetType()==D3DRTYPE_TEXTURE && destination->GetType()==D3DRTYPE_TEXTURE)
        TransferAtlas((IDirect3DTexture9*)source,(IDirect3DTexture9*)destination);
    return hr;
}
HRESULT WINAPI HookUpdateSurface(IDirect3DDevice9* device,IDirect3DSurface9* source,const RECT* area,IDirect3DSurface9* destination,const POINT* point) {
    DeviceFunctions functions{};if(!OriginalFunctions(deviceFunctions,device,functions)) return D3DERR_INVALIDCALL;
    IDirect3DTexture9* src=nullptr;IDirect3DTexture9* dst=nullptr;bool whole=false;
    if(destination) destination->GetContainer(IID_IDirect3DTexture9,(void**)&dst);
    if(source && destination && (!point || (!point->x && !point->y))) {
        D3DSURFACE_DESC desc{};
        if(SUCCEEDED(source->GetDesc(&desc)) && desc.Format==D3DFMT_DXT5 &&
           (!area || (!area->left && !area->top && area->right==(LONG)desc.Width && area->bottom==(LONG)desc.Height))) {
            source->GetContainer(IID_IDirect3DTexture9,(void**)&src);whole=true;
            if(src && dst) {
                TrackTexture(src,desc.Width,desc.Height,desc.Format);auto state=FindTexture(src);bool needsRead=false;
                if(state) {std::lock_guard<std::recursive_mutex> guard(hookMutex);needsRead=!state->atlas;}
                if(needsRead) {D3DLOCKED_RECT locked{};if(SUCCEEDED(src->LockRect(0,&locked,nullptr,D3DLOCK_READONLY))) src->UnlockRect(0);}
            }
        }
    }
    HRESULT hr=functions.surface(device,source,area,destination,point);
    TexturePtr retired;
    if(SUCCEEDED(hr) && dst) {
        if(whole && src) TransferAtlas(src,dst);
        else {auto state=FindTexture(dst);if(state) {std::lock_guard<std::recursive_mutex> guard(hookMutex);retired.swap(state->replacement);state->atlas.reset();state->replacementFailed=false;state->revision++;}}
    }
    if(src) src->Release();if(dst) dst->Release();return hr;
}
HRESULT WINAPI HookSetTexture(IDirect3DDevice9* device,DWORD stage,IDirect3DBaseTexture9* base) {
    DeviceFunctions functions{};if(!OriginalFunctions(deviceFunctions,device,functions)) return D3DERR_INVALIDCALL;
    auto state=FindTexture((IDirect3DTexture9*)base);
    // Only known font-sized uploads enter the replacement path. Other scene
    // and Scaleform textures pass through without any driver queries/readback.
    // Keep known font bindings valid when UE3 flushes deferred text batches.
    if(!state && !nativeDrawDepth) return functions.set(device,stage,base);
    if(base && !state && base->GetType()==D3DRTYPE_TEXTURE) {
        auto texture=(IDirect3DTexture9*)base;D3DSURFACE_DESC desc{};InterlockedIncrement(&lateInspections);
        if(SUCCEEDED(texture->GetLevelDesc(0,&desc))) {
            TrackTexture(texture,desc.Width,desc.Height,desc.Format);state=FindTexture(texture);
            if(state) {
                D3DLOCKED_RECT locked{};
                if(SUCCEEDED(texture->LockRect(0,&locked,nullptr,D3DLOCK_READONLY))) texture->UnlockRect(0);
            }
        }
    }
    TexturePtr replacement;std::shared_ptr<Atlas> atlas;uint64_t revision=0;
    if(state) {
        std::lock_guard<std::recursive_mutex> guard(hookMutex);
        replacement=state->replacement;
        if(state->atlas && !replacement && !state->replacementFailed) {
            state->replacementFailed=true;atlas=state->atlas;revision=state->revision;
        }
    }
    if(atlas) {
        replacement=CreateReplacement(device,*atlas,functions);bool current;
        {
            std::lock_guard<std::recursive_mutex> guard(hookMutex);
            current=state->atlas==atlas && state->revision==revision;
            if(current && replacement) {state->replacement=replacement;state->replacementFailed=false;}
        }
        if(!current) replacement.reset();
    }
    IDirect3DBaseTexture9* bound=replacement?replacement.get():base;
    HRESULT hr=functions.set(device,stage,bound);
    if(FAILED(hr) && replacement) {
        TexturePtr retired;
        if(state) {
            std::lock_guard<std::recursive_mutex> guard(hookMutex);
            if(state->replacement==replacement) {state->replacementFailed=true;retired.swap(state->replacement);}
        }
        hr=functions.set(device,stage,base);bound=base;
        Note("Native font binding rejected; reverted to the original bitmap texture.\r\n");
    }
    if(SUCCEEDED(hr) && bound!=base && InterlockedCompareExchange(&bindLogged,1,0)==0) Note("Native 4x font texture bound successfully. Original game renderer and glyph metrics retained.\r\n");
    return hr;
}
bool HookDeviceTable(IDirect3DDevice9* device) {
    std::lock_guard<std::recursive_mutex> guard(hookMutex);
    void** table=Table(device);if(deviceFunctions.count(table)) return true;
    DeviceFunctions functions{(CreateTextureFn)table[23],(SetTextureFn)table[65],(UpdateTextureFn)table[31],(UpdateSurfaceFn)table[30]};deviceFunctions.emplace(table,functions);
    if(WriteSlot(table+23,(void*)HookCreateTexture) && WriteSlot(table+65,(void*)HookSetTexture) && WriteSlot(table+31,(void*)HookUpdateTexture) && WriteSlot(table+30,(void*)HookUpdateSurface)) return true;
    WriteSlot(table+23,(void*)functions.create);WriteSlot(table+65,(void*)functions.set);WriteSlot(table+31,(void*)functions.update);WriteSlot(table+30,(void*)functions.surface);deviceFunctions.erase(table);return false;
}
HRESULT WINAPI HookCreateDevice(IDirect3D9* d3d,UINT adapter,D3DDEVTYPE type,HWND window,DWORD behavior,D3DPRESENT_PARAMETERS* params,IDirect3DDevice9** out) {
    CreateDeviceFn original=nullptr;if(!OriginalFunctions(factoryFunctions,d3d,original)) return D3DERR_INVALIDCALL;
    HRESULT hr=original(d3d,adapter,type,window,behavior,params,out);
    if(SUCCEEDED(hr) && out && *out) {
        if(HookDeviceTable(*out)) Note("Game Direct3D9 device intercepted; native font texture hooks active.\r\n");
        else Note("Direct3D9 device hook failed.\r\n");
    }
    return hr;
}
} // namespace

void EnterNativeFontDraw() {nativeDrawDepth++;}
void LeaveNativeFontDraw() {if(nativeDrawDepth) nativeDrawDepth--;}

bool InstallNativeFontHooks(FontLog log) {
    logger=log;
    IDirect3D9* factory=Direct3DCreate9(D3D_SDK_VERSION);if(!factory) {Note("Direct3D9 factory unavailable.\r\n");return false;}
    void** table=Table(factory);bool installed=true;
    {
        std::lock_guard<std::recursive_mutex> guard(hookMutex);
        if(!factoryFunctions.count(table)) {
            factoryFunctions.emplace(table,(CreateDeviceFn)table[16]);
            if(!WriteSlot(table+16,(void*)HookCreateDevice)) {factoryFunctions.erase(table);installed=false;}
        }
    }
    // DirectInput initializes this mod before the supported game creates its
    // device. Hook the real CreateDevice call; do not create a second HAL device
    // while the game's fullscreen renderer is being initialized.
    factory->Release();
    if(installed) Note("Native font factory hook installed; awaiting the game graphics device (no probe device).\r\n");
    return installed;
}

namespace {
HRESULT WINAPI FailTextureCreate(IDirect3DDevice9*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9** out,HANDLE*) {
    if(out) *out=nullptr;return D3DERR_OUTOFVIDEOMEMORY;
}
SetTextureFn bindingDelegate;
SetTextureFn unlockedBindingDelegate;
CreateTextureFn unlockedCreationDelegate;
void AssertDriverMutexFree() {
    bool free=false;
    std::thread probe([&] {free=hookMutex.try_lock();if(free) hookMutex.unlock();});probe.join();
    assert(free);
}
HRESULT WINAPI CheckUnlockedBinding(IDirect3DDevice9* device,DWORD stage,IDirect3DBaseTexture9* base) {
    AssertDriverMutexFree();return unlockedBindingDelegate(device,stage,base);
}
HRESULT WINAPI CheckUnlockedCreation(IDirect3DDevice9* device,UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** out,HANDLE* shared) {
    AssertDriverMutexFree();return unlockedCreationDelegate(device,w,h,levels,usage,format,pool,out,shared);
}
HRESULT WINAPI RejectNativeBinding(IDirect3DDevice9* device,DWORD stage,IDirect3DBaseTexture9* base) {
    if(base && base->GetType()==D3DRTYPE_TEXTURE) {D3DSURFACE_DESC d{};if(SUCCEEDED(((IDirect3DTexture9*)base)->GetLevelDesc(0,&d)) && d.Format==D3DFMT_A8R8G8B8) return D3DERR_INVALIDCALL;}
    return bindingDelegate(device,stage,base);
}
}
bool TestNativeFontGPU(FontLog log) {
    logger=log;if(atlases.empty()) return false;
    if(!InstallNativeFontHooks(log)) return false;
    IDirect3D9* factory=Direct3DCreate9(D3D_SDK_VERSION);if(!factory) return false;
    HWND window=CreateWindowExW(0,L"STATIC",L"ArkhamSubtitleScale GPU test",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=1;pp.BackBufferHeight=1;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=window;
    IDirect3DDevice9* device=nullptr;
    HRESULT hr=factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&device);
    factory->Release();if(FAILED(hr) || !device) {DestroyWindow(window);Note("GPU test device unavailable.\r\n");return false;}
    auto a=atlases.front();unsigned row=((a->width+3)/4)*16,rows=(a->height+3)/4;
    auto make=[&](D3DPOOL pool) {
        IDirect3DTexture9* t=nullptr;assert(SUCCEEDED(device->CreateTexture(a->width,a->height,1,0,D3DFMT_DXT5,pool,&t,nullptr)) && t);return t;
    };
    auto upload=[&](IDirect3DTexture9* t,bool change,const RECT* area=nullptr) {
        D3DLOCKED_RECT r{};assert(SUCCEEDED(t->LockRect(0,&r,area,0)));
        if(!area) {for(unsigned y=0;y<rows;y++) memcpy((BYTE*)r.pBits+(size_t)y*r.Pitch,a->source.data()+(size_t)y*row,row);if(change) ((BYTE*)r.pBits)[0]^=1;}
        assert(SUCCEEDED(t->UnlockRect(0)));
    };
    auto bound=[&]() {IDirect3DBaseTexture9* b=nullptr;assert(SUCCEEDED(device->GetTexture(0,&b)));return b;};
    auto source=make(D3DPOOL_MANAGED);upload(source,false);
    assert(textures.at(source)->atlas==a);
    auto& functions=deviceFunctions.at(Table(device));
    unlockedBindingDelegate=functions.set;unlockedCreationDelegate=functions.create;
    functions.set=CheckUnlockedBinding;functions.create=CheckUnlockedCreation;
    assert(SUCCEEDED(device->SetTexture(0,source)));auto b=bound();assert(b && b!=source);
    functions.set=unlockedBindingDelegate;functions.create=unlockedCreationDelegate;
    D3DSURFACE_DESC desc{};assert(SUCCEEDED(((IDirect3DTexture9*)b)->GetLevelDesc(0,&desc)) && desc.Width==a->width*kResolution && desc.Height==a->height*kResolution && desc.Format==D3DFMT_A8R8G8B8);
    D3DLOCKED_RECT gpu{};assert(SUCCEEDED(((IDirect3DTexture9*)b)->LockRect(0,&gpu,nullptr,D3DLOCK_READONLY)));
    for(unsigned y=0;y<desc.Height;y++) assert(!memcmp((BYTE*)gpu.pBits+(size_t)y*gpu.Pitch,a->pixels.data()+(size_t)y*desc.Width,(size_t)desc.Width*4));
    assert(SUCCEEDED(((IDirect3DTexture9*)b)->UnlockRect(0)));b->Release();device->SetTexture(0,nullptr);
    // Ordinary menu/scene textures must not be inspected on every binding.
    IDirect3DTexture9* menu=nullptr;
    assert(SUCCEEDED(device->CreateTexture(64,64,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&menu,nullptr)) && menu);
    assert(!FindTexture(menu));LONG inspected=lateInspections;
    LARGE_INTEGER frequency,start,stop;QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<20000;i++) assert(SUCCEEDED(functions.set(device,0,i&1?menu:nullptr)));
    QueryPerformanceCounter(&stop);double originalMs=1000.0*(stop.QuadPart-start.QuadPart)/frequency.QuadPart;
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<20000;i++) assert(SUCCEEDED(device->SetTexture(0,i&1?menu:nullptr)));
    QueryPerformanceCounter(&stop);double hookedMs=1000.0*(stop.QuadPart-start.QuadPart)/frequency.QuadPart;
    assert(lateInspections==inspected && !FindTexture(menu));b=bound();assert(b==menu);b->Release();
    device->SetTexture(0,nullptr);menu->Release();
    char benchmark[200];sprintf_s(benchmark,"PASS: 20000 unrelated texture bindings: original %.2f ms, hooked %.2f ms; zero late inspections/readbacks.\r\n",originalMs,hookedMs);Note(benchmark);
    // A font uploaded before tracking is inspected only during explicit text
    // drawing. Later deferred batches can use its cached native replacement.
    IDirect3DTexture9* late=nullptr;
    assert(SUCCEEDED(functions.create(device,a->width,a->height,1,0,D3DFMT_DXT5,D3DPOOL_MANAGED,&late,nullptr)) && late);
    upload(late,false);assert(!FindTexture(late));
    assert(SUCCEEDED(device->SetTexture(0,late)));b=bound();assert(b==late);b->Release();assert(lateInspections==inspected);
    {
        NativeFontDrawScope explicitText;
        assert(SUCCEEDED(device->SetTexture(0,late)));b=bound();assert(b && b!=late);b->Release();
    }
    assert(lateInspections==inspected+1 && FindTexture(late)->atlas==a);
    assert(SUCCEEDED(device->SetTexture(0,late)));b=bound();assert(b && b!=late);b->Release();assert(lateInspections==inspected+1);
    device->SetTexture(0,nullptr);late->Release();
    Note("PASS: driver creation/binding callbacks run without the metadata mutex; late font recognition is scoped and deferred bindings use cached atlases.\r\n");
    // Changed source must not be mistaken for a font, or keep a stale atlas.
    upload(source,true);assert(!textures.at(source)->atlas);assert(SUCCEEDED(device->SetTexture(0,source)));b=bound();assert(b==source);b->Release();device->SetTexture(0,nullptr);
    // Recognize a fresh upload again, then force allocation failure: keep the
    // original bitmap bound, with no repeated allocation attempts per frame.
    upload(source,false);auto create=functions.create;functions.create=FailTextureCreate;
    assert(SUCCEEDED(device->SetTexture(0,source)));functions.create=create;b=bound();assert(b==source);b->Release();assert(textures.at(source)->replacementFailed);
    assert(SUCCEEDED(device->SetTexture(0,source)));b=bound();assert(b==source);b->Release();device->SetTexture(0,nullptr);
    upload(source,false);bindingDelegate=functions.set;functions.set=RejectNativeBinding;
    assert(SUCCEEDED(device->SetTexture(0,source)));functions.set=bindingDelegate;b=bound();assert(b==source);b->Release();assert(textures.at(source)->replacementFailed && !textures.at(source)->replacement);device->SetTexture(0,nullptr);
    // A partial lock cannot identify a font from uninitialized/partial bytes.
    auto partial=make(D3DPOOL_MANAGED);RECT area{0,0,4,4};upload(partial,false,&area);assert(!textures.at(partial)->atlas);partial->Release();
    // System-memory staging upload -> default-pool GPU texture.
    auto staged=make(D3DPOOL_SYSTEMMEM),destination=make(D3DPOOL_DEFAULT);upload(staged,false);
    assert(SUCCEEDED(device->UpdateTexture(staged,destination)));assert(textures.at(destination)->atlas==a);
    assert(SUCCEEDED(device->SetTexture(0,destination)));b=bound();assert(b && b!=destination);b->Release();device->SetTexture(0,nullptr);
    // Also cover surface transfers used by some upload implementations.
    auto surfaceDestination=make(D3DPOOL_DEFAULT);IDirect3DSurface9* from=nullptr;IDirect3DSurface9* to=nullptr;
    assert(SUCCEEDED(staged->GetSurfaceLevel(0,&from)));assert(SUCCEEDED(surfaceDestination->GetSurfaceLevel(0,&to)));
    assert(SUCCEEDED(device->UpdateSurface(from,nullptr,to,nullptr)));assert(textures.at(surfaceDestination)->atlas==a);from->Release();to->Release();
    upload(staged,true);assert(SUCCEEDED(device->UpdateTexture(staged,destination)));assert(!textures.at(destination)->atlas && !textures.at(destination)->replacement);
    source->Release();staged->Release();destination->Release();surfaceDestination->Release();
    for(const auto& next:atlases) {
        a=next;row=((a->width+3)/4)*16;rows=(a->height+3)/4;
        auto localized=make(D3DPOOL_MANAGED);upload(localized,false);assert(textures.at(localized)->atlas==a);
        assert(SUCCEEDED(device->SetTexture(0,localized)));b=bound();assert(b && b!=localized);
        assert(SUCCEEDED(((IDirect3DTexture9*)b)->GetLevelDesc(0,&desc)) && desc.Width==a->width*kResolution && desc.Height==a->height*kResolution);
        assert(SUCCEEDED(((IDirect3DTexture9*)b)->LockRect(0,&gpu,nullptr,D3DLOCK_READONLY)));
        for(unsigned y=0;y<desc.Height;y++) assert(!memcmp((BYTE*)gpu.pBits+(size_t)y*gpu.Pitch,a->pixels.data()+(size_t)y*desc.Width,(size_t)desc.Width*4));
        assert(SUCCEEDED(((IDirect3DTexture9*)b)->UnlockRect(0)));b->Release();device->SetTexture(0,nullptr);localized->Release();
    }
    assert(textures.empty());assert(SUCCEEDED(device->Reset(&pp)));device->Release();DestroyWindow(window);
    Note("PASS: real Direct3D9 font upload/binding for all localization atlases, pixel equality, changed/partial texture rejection, staged texture/surface transfers, allocation/binding failure bitmap fallback, resource release and device reset.\r\n");return true;
}
