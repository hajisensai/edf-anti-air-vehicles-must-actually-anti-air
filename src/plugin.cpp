// EDF6AutoTurret: a Vehicle603_Flak whose guns carry a lock-on profile (LockonType != 0)
// slews its turret onto the guns' current lock target by itself, preferring air targets.
// The rider keeps the trigger; moving the aim stick takes the turret back for a moment.
// All addresses are RVAs into EDF.dll TimeDateStamp 0x678CCB46; see docs/re-notes.md.
#include <Windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#pragma warning(push)
#pragma warning(disable:4201)
#include "PluginAPI.h"
#pragma warning(pop)
#include "memory.h"

namespace autoturret {
unsigned char* image=nullptr;
HMODULE module=nullptr;
wchar_t logPath[MAX_PATH]{};
wchar_t iniPath[MAX_PATH]{};

struct Config {
    bool enabled=true;
    bool debug=false;
    float gain=3.0f;           // stick input per radian of aim error, clamped to +-1
    float yawSign=1.0f;        // axis angle = sign * geometric angle + offset
    float yawOffset=0.0f;
    float pitchSign=1.0f;
    float pitchOffset=0.0f;
    float pivotHeight=2.5f;    // turret pivot above the vehicle origin, metres
    float airHeight=12.0f;     // a target this far above the pivot counts as air
    float bulletSpeed=0.0f;    // metres per frame for lead; 0 disables lead
    float overrideDeadzone=0.2f;
    DWORD overrideMs=800;      // manual stick input suspends auto-aim this long
};
Config cfg{};
FILETIME iniStamp{};
ULONGLONG iniCheckedAt=0;

// --- EDF.dll layout ---
constexpr unsigned kFlakVtable=0x17DC620,kFlakInput=0x621460;   // Vehicle603_Flak, slot 55
constexpr std::size_t kInputSlot=55;
// Vehicle
constexpr std::size_t kMatrix=0x60,kPosition=0x90,kDead=0x2E8,kSeats=0x608,kSeatCount=0x618,kTurn=0x2AA0;
// Seat (stride 0x340): weapon holders, aim controller, rider stick
constexpr std::size_t kSeatStride=0x340,kSeatWeapons=0xC8,kSeatWeaponCount=0xD8,kSeatAim=0xE0,kStick=0x2D0;
constexpr std::size_t kHolderWeapon=0x10;
// VehicleWeaponAim: axes at +0x10, stride 0x40; {min, max, angle, velocity, ...}
constexpr std::size_t kAimAxes=0x10,kAxisStride=0x40,kAxisMin=0x0,kAxisMax=0x4,kAxisAngle=0x8;
// Weapon: lock-on profile and std::list<{weak_ptr target, float time}>
constexpr std::size_t kLockonType=0x6B0,kLockList=0xC60;
constexpr std::size_t kNodeTarget=0x10,kNodeCtrl=0x18;
// GameObjectBase aim point: a body node list when present, else the origin
constexpr std::size_t kAimNodes=0x368,kAimNodeCount=0x378,kAimNodePos=0x10;
constexpr int kMaxLocks=64;
constexpr float kPi=3.14159265f;

using InputFn=void(__fastcall*)(void*,std::uintptr_t);
InputFn originalInput=nullptr;

struct Track {
    const void* vehicle;
    const void* target;
    float last[3];
    ULONGLONG at;          // last update tick
    ULONGLONG manualUntil;
    ULONGLONG loggedAt;
};
Track tracks[8]{};

// Once-a-second snapshot of where Steer stopped, for Debug=1.
struct Diag {
    ULONGLONG at;
    unsigned calls,ridden;
    unsigned weapons,locked,listed,deadByte,expired;
    const char* stop;
};
Diag diag{};

void Log(const char* format,...) noexcept {
    if(!logPath[0])return;
    char text[1000]{};va_list args;va_start(args,format);vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args);va_end(args);
    FILE* f=nullptr;if(_wfopen_s(&f,logPath,L"ab") || !f)return;
    SYSTEMTIME t{};GetLocalTime(&t);
    fprintf(f,"[%02u:%02u:%02u] %s\r\n",t.wHour,t.wMinute,t.wSecond,text);fclose(f);
}

template<class T> T At(const void* base,std::size_t offset) noexcept {
    T value;std::memcpy(&value,static_cast<const unsigned char*>(base)+offset,sizeof(T));return value;
}
template<class T> void Put(void* base,std::size_t offset,T value) noexcept {
    std::memcpy(static_cast<unsigned char*>(base)+offset,&value,sizeof(T));
}

float Dot(const float* a,const float* b) noexcept { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
float Clamp(float v,float lo,float hi) noexcept { return v<lo?lo:(v>hi?hi:v); }
float Wrap(float a) noexcept {
    while(a>kPi)a-=2*kPi;
    while(a<-kPi)a+=2*kPi;
    return a;
}

Track& TrackFor(const void* vehicle) noexcept {
    Track* slot=&tracks[0];
    for(auto& t:tracks) {
        if(t.vehicle==vehicle)return t;
        if(t.at<slot->at)slot=&t;
    }
    *slot=Track{};slot->vehicle=vehicle;
    return *slot;
}

bool Alive(const unsigned char* node) noexcept {
    const auto target=At<const unsigned char*>(node,kNodeTarget);
    const auto ctrl=At<const unsigned char*>(node,kNodeCtrl);
    if(!target || !Readable(ctrl,0x10) || At<std::int32_t>(ctrl,8)<=0){++diag.expired;return false;}
    if(!Readable(target,kAimNodeCount+8))return false;
    if(target[kDead])++diag.deadByte;
    return !target[kDead];
}

bool AimPoint(const unsigned char* target,float* out) noexcept {
    const float* p=reinterpret_cast<const float*>(target+kPosition);
    if(At<std::uint64_t>(target,kAimNodeCount)) {
        const auto nodes=At<const unsigned char* const*>(target,kAimNodes);
        if(Readable(nodes,8) && Readable(nodes[0],kAimNodePos+12))
            p=reinterpret_cast<const float*>(nodes[0]+kAimNodePos);
    }
    for(int i=0;i<3;++i){out[i]=p[i];if(!std::isfinite(out[i]))return false;}
    return true;
}

// Target position in the vehicle's frame, measured from the turret pivot.
void ToLocal(const unsigned char* vehicle,const float* world,float* local) noexcept {
    const float* m=reinterpret_cast<const float*>(vehicle+kMatrix);
    const float d[3]={world[0]-m[12],world[1]-m[13],world[2]-m[14]};
    local[0]=Dot(d,m);local[1]=Dot(d,m+4)-cfg.pivotHeight;local[2]=Dot(d,m+8);
}

// Best lock across the seat's guns: any air target beats any ground one, nearest first.
const unsigned char* PickTarget(const unsigned char* vehicle,const unsigned char* seat,bool& armed,float* local) noexcept {
    armed=false;
    const unsigned char* best=nullptr;float bestScore=0.0f;
    const auto holders=At<const unsigned char* const*>(seat,kSeatWeapons);
    const auto count=At<std::uint64_t>(seat,kSeatWeaponCount);
    if(count>8 || !Readable(holders,count*8))return nullptr;
    for(std::uint64_t i=0;i<count;++i) {
        if(!Readable(holders[i],kHolderWeapon+8))continue;
        const auto weapon=At<const unsigned char*>(holders[i],kHolderWeapon);
        if(!Readable(weapon,kLockList+0x10) || !At<std::int32_t>(weapon,kLockonType))continue;
        armed=true;++diag.weapons;
        diag.listed+=static_cast<unsigned>(At<std::uint64_t>(weapon,kLockList+8));
        const auto head=At<const unsigned char*>(weapon,kLockList);
        if(!Readable(head,0x10))continue;
        const unsigned char* node=At<const unsigned char*>(head,0);
        for(int n=0;n<kMaxLocks && node!=head && Readable(node,0x28);++n,node=At<const unsigned char*>(node,0)) {
            if(!Alive(node))continue;
            ++diag.locked;
            const auto target=At<const unsigned char*>(node,kNodeTarget);
            float world[3],l[3];
            if(!AimPoint(target,world))continue;
            ToLocal(vehicle,world,l);
            const float distance=std::sqrt(Dot(l,l));
            const float score=distance+(l[1]>cfg.airHeight ? 0.0f : 1.0e6f);
            if(!best || score<bestScore){best=target;bestScore=score;std::memcpy(local,l,sizeof(l));}
        }
    }
    return best;
}

void Lead(const unsigned char* vehicle,Track& track,const unsigned char* target,float* local) noexcept {
    float world[3];
    if(!AimPoint(target,world))return;
    const auto now=GetTickCount64();
    if(track.target==target && cfg.bulletSpeed>0.0f && now-track.at<200) {
        // Per-frame target velocity from the last sample; the frame is ~1/60 s.
        const float frames=Clamp((now-track.at)/16.6667f,1.0f,12.0f);
        const float v[3]={(world[0]-track.last[0])/frames,(world[1]-track.last[1])/frames,(world[2]-track.last[2])/frames};
        const float t=std::sqrt(Dot(local,local))/cfg.bulletSpeed;
        const float ahead[3]={world[0]+v[0]*t,world[1]+v[1]*t,world[2]+v[2]*t};
        ToLocal(vehicle,ahead,local);
    }
    track.target=target;std::memcpy(track.last,world,sizeof(world));
}

void Steer(unsigned char* vehicle) noexcept {
    diag.stop="vehicle";
    if(!Readable(vehicle,kTurn+0x10,true) || vehicle[kDead] || At<std::uint32_t>(vehicle,kSeatCount)==0)return;
    const auto seat=At<const unsigned char*>(vehicle,kSeats);
    diag.stop="seat";
    if(!Readable(seat,kSeatStride))return;
    Track& track=TrackFor(vehicle);
    const auto now=GetTickCount64();
    const float stickX=At<float>(seat,kStick),stickY=At<float>(seat,kStick+4);
    if(std::fabs(stickX)>cfg.overrideDeadzone || std::fabs(stickY)>cfg.overrideDeadzone)track.manualUntil=now+cfg.overrideMs;
    bool armed=false;float local[3]{};
    const auto target=PickTarget(vehicle,seat,armed,local);
    if(!armed){diag.stop="unarmed";return;}       // a stock flak: leave it alone
    if(now<track.manualUntil){diag.stop="manual";track.at=now;track.target=nullptr;return;}
    if(!target){diag.stop="no-target";track.at=now;track.target=nullptr;return;}
    diag.stop="aiming";
    Lead(vehicle,track,target,local);
    track.at=now;
    const auto axes=seat+kSeatAim+kAimAxes;
    const float yaw=At<float>(axes,kAxisAngle),pitch=At<float>(axes+kAxisStride,kAxisAngle);
    const float wantYaw=cfg.yawSign*std::atan2(local[0],local[2])+cfg.yawOffset;
    const float flat=std::sqrt(local[0]*local[0]+local[2]*local[2]);
    float wantPitch=cfg.pitchSign*std::atan2(local[1],flat)+cfg.pitchOffset;
    wantPitch=Clamp(wantPitch,At<float>(axes+kAxisStride,kAxisMin),At<float>(axes+kAxisStride,kAxisMax));
    const bool fullCircle=At<float>(axes,kAxisMax)-At<float>(axes,kAxisMin)>=2*kPi-0.01f;
    const float yawError=fullCircle ? Wrap(wantYaw-yaw) : wantYaw-yaw;
    const float in[2]={Clamp(yawError*cfg.gain,-1.0f,1.0f),Clamp((wantPitch-pitch)*cfg.gain,-1.0f,1.0f)};
    Put<float>(vehicle,kTurn,in[0]);Put<float>(vehicle,kTurn+4,in[1]);
    if(cfg.debug && now-track.loggedAt>500) {
        track.loggedAt=now;
        Log("AIM v=%p t=%p local=(%.1f,%.1f,%.1f) yaw=%.3f->%.3f pitch=%.3f->%.3f in=(%.2f,%.2f)",
            vehicle,target,local[0],local[1],local[2],yaw,wantYaw,pitch,wantPitch,in[0],in[1]);
    }
}

void ReloadConfigIfChanged() noexcept;

void FlushDiag(const void* vehicle) noexcept {
    const auto now=GetTickCount64();
    if(!cfg.debug || now-diag.at<1000)return;
    if(diag.at)Log("DIAG v=%p calls=%u ridden=%u weapons=%u listed=%u locked=%u dead=%u expired=%u last=%s",
        vehicle,diag.calls,diag.ridden,diag.weapons,diag.listed,diag.locked,diag.deadByte,diag.expired,
        diag.stop?diag.stop:"-");
    diag=Diag{};diag.at=now;
}

void __fastcall HookInput(void* vehicle,std::uintptr_t hasInput) {
    originalInput(vehicle,hasInput);
    ++diag.calls;
    FlushDiag(vehicle);
    if(!(hasInput&0xFF))return;             // no rider: the stock code just zeroed the turn
    ++diag.ridden;
    ReloadConfigIfChanged();
    if(!cfg.enabled)return;
    __try { Steer(static_cast<unsigned char*>(vehicle)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {diag.stop="fault";}
}

bool Matches(std::size_t rva,const unsigned char* bytes,std::size_t size) noexcept {
    return std::memcmp(image+rva,bytes,size)==0;
}

bool CheckProfile(HMODULE handle) noexcept {
    __try {
        auto base=reinterpret_cast<unsigned char*>(handle);
        if(!Readable(base,0x1000))return false;
        auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>0x800)return false;
        auto pe=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64
           || pe->FileHeader.TimeDateStamp!=0x678CCB46 || pe->OptionalHeader.SizeOfImage!=0x22CE000)return false;
        image=base;
        const unsigned char input[]={0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x0F,0xB6,0xDA};
        const unsigned char stick[]={0xF3,0x0F,0x10,0x83,0xD0,0x02,0x00,0x00};   // movss xmm0,[rbx+2D0]
        const unsigned char turn[]={0xF3,0x0F,0x11,0x87,0xA0,0x2A,0x00,0x00};    // movss [rdi+2AA0],xmm0
        const unsigned char apply[]={0x48,0x8D,0x93,0xA0,0x2A,0x00,0x00};        // lea rdx,[rbx+2AA0] (slot 4)
        const unsigned char lockType[]={0x89,0x86,0xB0,0x06,0x00,0x00};          // mov [rsi+6B0],eax
        const bool ok=Matches(kFlakInput,input,sizeof(input)) && Matches(0x6214D8,stick,sizeof(stick))
            && Matches(0x6214E7,turn,sizeof(turn)) && Matches(0x621872,apply,sizeof(apply))
            && Matches(0x68D124,lockType,sizeof(lockType))
            && reinterpret_cast<void**>(image+kFlakVtable)[kInputSlot]==image+kFlakInput;
        if(!ok)image=nullptr;
        return ok;
    } __except(EXCEPTION_EXECUTE_HANDLER){image=nullptr;return false;}
}

// Swap one vtable entry, only if it still holds the expected function.
bool PatchVtableSlot(void** slot,void* expected,void* replacement) noexcept {
    DWORD old=0;
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    const bool ok=InterlockedCompareExchangePointer(slot,replacement,expected)==expected;
    VirtualProtect(slot,sizeof(void*),old,&old);
    return ok;
}

float ReadFloat(const wchar_t* key,float fallback) noexcept {
    wchar_t text[64]{};
    GetPrivateProfileStringW(L"AutoTurret",key,L"",text,64,iniPath);
    wchar_t* end=nullptr;
    const float value=std::wcstof(text,&end);
    return end!=text && std::isfinite(value) ? value : fallback;
}

void LoadConfig() noexcept {
    Config next{};
    next.enabled=GetPrivateProfileIntW(L"AutoTurret",L"Enabled",1,iniPath)!=0;
    next.debug=GetPrivateProfileIntW(L"AutoTurret",L"Debug",0,iniPath)!=0;
    next.gain=ReadFloat(L"Gain",next.gain);
    next.yawSign=ReadFloat(L"YawSign",next.yawSign);
    next.yawOffset=ReadFloat(L"YawOffset",next.yawOffset);
    next.pitchSign=ReadFloat(L"PitchSign",next.pitchSign);
    next.pitchOffset=ReadFloat(L"PitchOffset",next.pitchOffset);
    next.pivotHeight=ReadFloat(L"PivotHeight",next.pivotHeight);
    next.airHeight=ReadFloat(L"AirHeight",next.airHeight);
    next.bulletSpeed=ReadFloat(L"BulletSpeed",next.bulletSpeed);
    next.overrideDeadzone=ReadFloat(L"OverrideDeadzone",next.overrideDeadzone);
    next.overrideMs=GetPrivateProfileIntW(L"AutoTurret",L"OverrideMs",next.overrideMs,iniPath);
    cfg=next;
    Log("CONFIG enabled=%d debug=%d gain=%.2f yaw=%.0f%+.3f pitch=%.0f%+.3f pivot=%.1f air=%.1f bullet=%.2f override=%.2f/%lums",
        cfg.enabled,cfg.debug,cfg.gain,cfg.yawSign,cfg.yawOffset,cfg.pitchSign,cfg.pitchOffset,cfg.pivotHeight,
        cfg.airHeight,cfg.bulletSpeed,cfg.overrideDeadzone,cfg.overrideMs);
}

FILETIME IniStamp() noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(iniPath,GetFileExInfoStandard,&data) ? data.ftLastWriteTime : FILETIME{};
}

// Edits to the ini apply within a second; no game restart needed.
void ReloadConfigIfChanged() noexcept {
    const auto now=GetTickCount64();
    if(now-iniCheckedAt<1000)return;
    iniCheckedAt=now;
    const auto stamp=IniStamp();
    if(CompareFileTime(&stamp,&iniStamp)==0)return;
    iniStamp=stamp;
    LoadConfig();
}
}  // namespace autoturret

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo* info) {
    using namespace autoturret;
    if(!info)return false;
    GetModuleFileNameW(module,iniPath,MAX_PATH);
    auto dot=wcsrchr(iniPath,L'.');if(!dot)return false;
    wcscpy_s(dot,MAX_PATH-(dot-iniPath),L".ini");
    wcscpy_s(logPath,iniPath);
    dot=wcsrchr(logPath,L'.');wcscpy_s(dot,MAX_PATH-(dot-logPath),L".log");
    info->infoVersion=PluginInfo::MaxInfoVer;info->name="EDF6 Auto Turret";info->version=PLUG_VER(0,1,0,0);
    Log("EDF6AutoTurret 0.1.0 loading");
    iniStamp=IniStamp();
    LoadConfig();
    if(!CheckProfile(GetModuleHandleW(L"EDF.dll"))){Log("REFUSED: unsupported EDF.dll or conflicting patch");return false;}
    originalInput=reinterpret_cast<InputFn>(image+kFlakInput);
    auto slot=reinterpret_cast<void**>(image+kFlakVtable)+kInputSlot;
    const bool hooked=PatchVtableSlot(slot,reinterpret_cast<void*>(originalInput),reinterpret_cast<void*>(&HookInput));
    Log("HOOK flak input slot=%d",hooked);
    return hooked;  // never unload code a patched slot points at
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){autoturret::module=instance;DisableThreadLibraryCalls(instance);}
    return TRUE;
}
