// EDF6AutoTurret: a Vehicle603_Flak whose guns carry a lock-on profile (LockonType != 0)
// slews its turret onto an enemy inside its tracking range by itself, preferring air targets;
// the rider keeps the trigger, and aims by hand only while nothing is in tracking range.
// Enemies come straight from the game's lock-target registry (every lockable enemy, all around),
// not from the guns' lock lists, which only cover the front hemisphere and churn.
// It also time-fuses the guns' shells to the target's range, proximity-fuses them near any enemy
// in range, and lets LockonType 4 guns fire without a lock (stock fire-start refuses lock-on
// weapons with an empty lock list; our guns have LockonRange 0 so they never lock).
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
    float pitchSign=-1.0f;     // the pitch axis is negative-up (Kepler: -60 deg up .. +5 deg down)
    float pitchOffset=0.0f;
    float pivotHeight=2.5f;    // turret pivot above the vehicle origin, metres
    float airHeight=12.0f;     // a target this far above the pivot counts as air
    bool lead=true;            // aim ahead of moving targets, using the gun's own AmmoSpeed
    float trackRange=0.75f;    // auto-aim only at targets within this share of the gun's range
    bool feedForward=true;     // drive the turret at the aim point's own angular rate, not only on the error
    float slewWeight=250.0f;   // metres a new target may be farther per radian it saves the turret turning
    bool fuse=true;            // set each round's lifetime (its burst point) to the target's range
    float fuseBias=0.0f;       // frames added to the computed fuse
    int fuseMin=4;             // never burst closer than this many frames
    float proximity=5.0f;      // burst when a round passes this close to a target (0 = off); keep below AmmoExplosion
    bool contact=true;         // burst the moment a round sticks to something or stops
    float burstVisual=2.5f;    // burst effect size as a multiple of the stock one (AmmoExplosion/5)
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
constexpr std::size_t kSeatStride=0x340,kSeatWeapons=0xC8,kSeatWeaponCount=0xD8,kSeatAim=0xE0;
constexpr std::size_t kHolderWeapon=0x10;
// VehicleWeaponAim: axes at +0x10, stride 0x40; {min, max, angle, velocity, ...}
constexpr std::size_t kAimAxes=0x10,kAxisStride=0x40,kAxisMin=0x0,kAxisMax=0x4,kAxisAngle=0x8;
// Weapon: lock-on profile (LockonType 4 marks our guns)
constexpr std::size_t kLockonType=0x6B0;
constexpr std::int32_t kOurLockonType=4;
// Weapon ammo parameters, copied into each round when it is fired
constexpr std::size_t kAmmoSpeed=0x894,kAmmoAlive=0x898;
// Lock-target registry: global pointer -> object holding std::list<{raw T*, weak_ptr}> at +8.
// Each T is one lock point of an object: +0 kind (0 = enemy kind), +8 the object, +0x10 its aim
// point (rewritten every frame from the bone matrix by 0x6C7700), +0x29 valid, +0x2A lockable.
// The lock query (0x696710) walks the same list; it is only touched on the game thread.
constexpr std::size_t kRegistry=0x20B2AB0,kRegList=0x8,kNodeTarget=0x10;
constexpr std::size_t kTargetObject=0x8,kTargetAim=0x10,kTargetValid=0x29,kTargetLockable=0x2A;
// Team relations: manager -> array (stride 0x38) per team -> int relation[team]; 2 = enemy.
constexpr std::size_t kTeams=0x20B2978,kTeamArray=0x38,kTeamStride=0x38,kTeamRelation=0x18;
constexpr std::size_t kTeam=0x314;
constexpr std::int32_t kEnemyRelation=2,kMaxTeam=64;
constexpr int kMaxEnemies=256,kMaxNodes=8192;
constexpr float kPi=3.14159265f;

using InputFn=void(__fastcall*)(void*,std::uintptr_t);
InputFn originalInput=nullptr;

// GrenadeBullet01 (the flak round): slot 1 deleting dtor, slot 5 per-frame update.
constexpr unsigned kGrenadeVtable=0x17A17E0,kGrenadeDtor=0x265B10,kGrenadeUpdate=0x264AB0;
constexpr std::size_t kGrenadeDtorSlot=1,kGrenadeUpdateSlot=5;
// Weapon slot 17 = "round spawned" (weapon, bullet), called once per round by fire 0x696FD0.
constexpr std::size_t kSpawnSlot=17;
// Bullet: weak-this control block, and the flight control block C at +0x140 with its flags,
// age/lifetime in frames (expires when age >= lifetime, 0x236899), position, velocity (m/s)
// and the stuck-to-something byte.
constexpr std::size_t kBulletWeakCtrl=0x30,kCtl=0x140;
constexpr std::size_t kCtlFlags=0xAF4,kCtlAge=0xAF8,kCtlAlive=0xA08,kCtlPos=0xB80,kCtlVel=0xB90,kCtlStuck=0xC00;
// Blast radius: the damage sphere uses C+0x788 (copied from AmmoExplosion at C+0xA20 at spawn,
// 0x2320A9), while the burst effect is drawn with size C+0xA20/5 (0x264B92). Scaling only C+0xA20
// right before the burst enlarges the fireball without touching the damage.
constexpr std::size_t kCtlBlast=0x788,kCtlBlastVisual=0xA20;
constexpr std::uint32_t kRoundDead=0x1,kRoundBurstOnExpiry=0x20;
constexpr float kStoppedSpeed=60.0f;     // m/s; the flak leaves the barrel at ~480

using SpawnFn=void(__fastcall*)(void*,void*);
using UpdateFn=void(__fastcall*)(void*,void*);
using DtorFn=void*(__fastcall*)(void*,unsigned);
UpdateFn originalUpdate=nullptr;
DtorFn originalDtor=nullptr;
struct SpawnPatch { const void* vtable; SpawnFn original; };
SpawnPatch spawnPatches[4]{};
bool proximityReady=false;

// Rounds fired by our guns, keyed by address and the bullet's own control block (address reuse).
struct Round { const void* bullet; const void* ctrl; };
constexpr int kMaxRounds=128;
Round rounds[kMaxRounds]{};
int roundNext=0;
SRWLOCK roundLock=SRWLOCK_INIT;

// Lock points of every enemy within gun range, sampled in the vehicle input phase that runs
// before the bullet update phase of the same frame. One enemy can own several entries.
struct Enemy { const void* object; float pos[3]; float origin[3]; };
constexpr ULONGLONG kEnemyMs=150;
Enemy enemies[kMaxEnemies]{};
int enemyCount=0;
ULONGLONG enemiesAt=0;

// The turret axes turn at (input x k) rad per frame; measured from the log at ~1.1 rad/s for a
// full input on both axes, and refined online from how far each axis actually moved.
constexpr float kPitchMargin=0.03f;   // rad past a pitch stop still counted as reachable
constexpr float kTurnPerInput=1.1f/60.0f,kTurnPerInputMin=0.2f/60.0f,kTurnPerInputMax=6.0f/60.0f;

struct Track {
    const void* vehicle;
    const void* target;
    float last[3];
    float vel[3];          // smoothed target velocity, metres per frame
    int frames;            // consecutive frames on this target
    float want[2];         // last wanted yaw/pitch
    float rate[2];         // smoothed change of the wanted angles, rad per frame
    float axis[2];         // last yaw/pitch angle
    float in[2];           // last input written
    float k[2];            // learned rad per frame per unit input
    ULONGLONG at;          // last update tick
    ULONGLONG loggedAt;
};
Track tracks[8]{};

// Once-a-second snapshot of where Steer stopped, for Debug=1.
struct Diag {
    ULONGLONG at;
    unsigned calls,ridden;
    unsigned weapons,enemies,registry;
    unsigned tagged,proximity,contact;
    float nearest;         // closest any tagged round came to an enemy aim point, metres
    const char* stop;
};
Diag diag{};

// Data AmmoAlive per gun, so the fuse can return to max range when nothing is tracked.
struct Fuse { const void* weapon; std::int32_t alive; };
Fuse fuses[16]{};

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

bool Finite(const unsigned char* base,std::size_t offset,float* out) noexcept {
    for(int i=0;i<3;++i){out[i]=At<float>(base,offset+i*4);if(!std::isfinite(out[i]))return false;}
    return true;
}

// Target position in the vehicle's frame, measured from the turret pivot.
void ToLocal(const unsigned char* vehicle,const float* world,float* local) noexcept {
    const float* m=reinterpret_cast<const float*>(vehicle+kMatrix);
    const float d[3]={world[0]-m[12],world[1]-m[13],world[2]-m[14]};
    local[0]=Dot(d,m);local[1]=Dot(d,m+4)-cfg.pivotHeight;local[2]=Dot(d,m+8);
}

// The relation row of `team`, or null. Called on the game thread inside Steer's __try.
const std::int32_t* Relations(std::int32_t team) noexcept {
    if(team<0 || team>=kMaxTeam)return nullptr;
    const auto manager=At<const unsigned char*>(image,kTeams);
    if(!Readable(manager,kTeamArray+8))return nullptr;
    const auto rows=At<const unsigned char*>(manager,kTeamArray);
    if(!Readable(rows+team*kTeamStride,kTeamStride))return nullptr;
    const auto relation=At<const std::int32_t*>(rows+team*kTeamStride,kTeamRelation);
    return Readable(relation,kMaxTeam*4) ? relation : nullptr;
}

// Snapshot every live, lockable enemy lock point within `range` of the turret pivot.
// Runs inside Steer's __try; the list is the game's own and is only changed on this thread.
void ScanEnemies(const unsigned char* vehicle,float range) noexcept {
    enemyCount=0;enemiesAt=GetTickCount64();
    const auto relation=Relations(At<std::int32_t>(vehicle,kTeam));
    const auto registry=At<const unsigned char*>(image,kRegistry);
    if(!relation || !Readable(registry,kRegList+0x10))return;
    const auto head=At<const unsigned char*>(registry,kRegList);
    if(!Readable(head,0x10))return;
    int n=0;
    for(auto node=At<const unsigned char*>(head,0);node!=head && n<kMaxNodes;node=At<const unsigned char*>(node,0),++n) {
        const auto target=At<const unsigned char*>(node,kNodeTarget);
        if(!target || target[0]!=0 || !target[kTargetValid] || !target[kTargetLockable])continue;
        const auto object=At<const unsigned char*>(target,kTargetObject);
        if(!object || object==vehicle || object[kDead])continue;
        const auto team=At<std::int32_t>(object,kTeam);
        if(team<0 || team>=kMaxTeam || relation[team]!=kEnemyRelation)continue;
        float world[3],origin[3],local[3];
        if(!Finite(target,kTargetAim,world) || !Finite(object,kPosition,origin))continue;
        ToLocal(vehicle,world,local);
        if(Dot(local,local)>range*range || enemyCount>=kMaxEnemies)continue;
        Enemy& e=enemies[enemyCount++];
        e.object=object;std::memcpy(e.pos,world,sizeof(world));std::memcpy(e.origin,origin,sizeof(origin));
    }
    diag.registry+=static_cast<unsigned>(n);diag.enemies+=static_cast<unsigned>(enemyCount);
}

void Tag(const void* bullet) noexcept {
    const Round round{bullet,At<const void*>(bullet,kBulletWeakCtrl)};
    AcquireSRWLockExclusive(&roundLock);
    Round* slot=nullptr;
    for(auto& r:rounds)if(!r.bullet){slot=&r;break;}
    if(!slot){slot=&rounds[roundNext];roundNext=(roundNext+1)%kMaxRounds;}   // full: drop the oldest
    *slot=round;
    ReleaseSRWLockExclusive(&roundLock);
    ++diag.tagged;
}

void Untag(const void* bullet) noexcept {
    AcquireSRWLockExclusive(&roundLock);
    for(auto& r:rounds)if(r.bullet==bullet)r=Round{};
    ReleaseSRWLockExclusive(&roundLock);
}

bool Tagged(const void* bullet) noexcept {
    const void* ctrl=At<const void*>(bullet,kBulletWeakCtrl);
    bool found=false;
    AcquireSRWLockShared(&roundLock);
    for(auto& r:rounds)if(r.bullet==bullet && r.ctrl==ctrl){found=true;break;}
    ReleaseSRWLockShared(&roundLock);
    return found;
}

void __fastcall SpawnHook(void* weapon,void* bullet) {
    const void* vtable=*static_cast<void**>(weapon);
    for(auto& p:spawnPatches)if(p.vtable==vtable){if(p.original)p.original(weapon,bullet);break;}
    __try {
        // Only our guns: LockonType 4 is unused by stock weapons (see PatchFireGate).
        if(bullet && At<std::int32_t>(weapon,kLockonType)==4 && *static_cast<void**>(bullet)==image+kGrenadeVtable)
            Tag(bullet);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

bool PatchVtableSlot(void** slot,void* expected,void* replacement) noexcept;

// Route the gun class's "round spawned" slot through SpawnHook, once per vtable.
void HookSpawn(const unsigned char* weapon) noexcept {
    if(!proximityReady)return;
    const auto vtable=At<void**>(weapon,0);
    SpawnPatch* free=nullptr;
    for(auto& p:spawnPatches) {
        if(p.vtable==vtable)return;
        if(!p.vtable && !free)free=&p;
    }
    if(!free || !Readable(vtable+kSpawnSlot,8))return;
    void* original=vtable[kSpawnSlot];
    free->original=reinterpret_cast<SpawnFn>(original);free->vtable=vtable;
    const bool ok=PatchVtableSlot(vtable+kSpawnSlot,original,reinterpret_cast<void*>(&SpawnHook));
    Log("HOOK spawn vtable=+0x%llX original=+0x%llX ok=%d",
        static_cast<unsigned long long>(reinterpret_cast<const unsigned char*>(vtable)-image),
        static_cast<unsigned long long>(static_cast<unsigned char*>(original)-image),ok);
}

// Closest approach of this frame's flight segment to the enemies seen this frame.
const Enemy* NearTarget(const float* pos,const float* step,float* burst) noexcept {
    if(GetTickCount64()-enemiesAt>kEnemyMs)return nullptr;
    const float stepLength=Dot(step,step);
    float best=1.0e18f;const Enemy* hit=nullptr;
    for(int i=0;i<enemyCount;++i) {
        const Enemy& m=enemies[i];
        const float d[3]={m.pos[0]-pos[0],m.pos[1]-pos[1],m.pos[2]-pos[2]};
        const float t=stepLength>1e-6f ? Clamp(Dot(d,step)/stepLength,0.0f,1.0f) : 0.0f;
        const float q[3]={pos[0]+step[0]*t,pos[1]+step[1]*t,pos[2]+step[2]*t};
        const float e[3]={m.pos[0]-q[0],m.pos[1]-q[1],m.pos[2]-q[2]};
        if(Dot(e,e)<best){best=Dot(e,e);hit=&m;std::memcpy(burst,q,sizeof(q));}
    }
    if(std::sqrt(best)<diag.nearest)diag.nearest=std::sqrt(best);
    return best<cfg.proximity*cfg.proximity ? hit : nullptr;
}

// Runs before the round's own update: a fused round gets its age set to its lifetime, so the
// stock expiry check in the same update bursts it at its (moved) position.
void Fuze(unsigned char* bullet) noexcept {
    unsigned char* c=bullet+kCtl;
    const auto flags=At<std::uint32_t>(c,kCtlFlags);
    if(flags&kRoundDead)return;
    const auto age=At<std::int32_t>(c,kCtlAge),alive=At<std::int32_t>(c,kCtlAlive);
    if(age<cfg.fuseMin || age>=alive)return;
    float pos[3],vel[3];
    for(int i=0;i<3;++i){pos[i]=At<float>(c,kCtlPos+i*4);vel[i]=At<float>(c,kCtlVel+i*4);}
    bool burst=false;
    if(cfg.contact && (c[kCtlStuck] || Dot(vel,vel)<kStoppedSpeed*kStoppedSpeed)){burst=true;++diag.contact;}
    if(!burst && cfg.proximity>0.0f) {
        const float step[3]={vel[0]/60.0f,vel[1]/60.0f,vel[2]/60.0f};
        float at[3];
        if(const Enemy* m=NearTarget(pos,step,at)) {
            burst=true;++diag.proximity;
            if(cfg.debug && diag.proximity<=4) {
                const float a[3]={m->pos[0]-at[0],m->pos[1]-at[1],m->pos[2]-at[2]};
                const float o[3]={m->origin[0]-at[0],m->origin[1]-at[1],m->origin[2]-at[2]};
                Log("BURST age=%d/%d aim=%.1fm origin=%.1fm blast=%.1fm",age,alive,std::sqrt(Dot(a,a)),std::sqrt(Dot(o,o)),At<float>(c,kCtlBlast));
            }
            for(int i=0;i<3;++i)Put<float>(c,kCtlPos+i*4,at[i]);
        }
    }
    if(burst || age+1>=alive)Put<float>(c,kCtlBlastVisual,At<float>(c,kCtlBlast)*cfg.burstVisual);   // bursting this update
    if(!burst)return;
    Put<std::uint32_t>(c,kCtlFlags,flags|kRoundBurstOnExpiry);
    Put<std::int32_t>(c,kCtlAge,alive);
}

void __fastcall UpdateHook(void* bullet,void* ctx) {
    bool ours=false;
    __try {
        ours=cfg.enabled && Tagged(bullet);
        if(ours)Fuze(static_cast<unsigned char*>(bullet));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    originalUpdate(bullet,ctx);
    if(!ours)return;
    __try { if(At<std::uint32_t>(bullet,kCtl+kCtlFlags)&kRoundDead)Untag(bullet); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void* __fastcall DtorHook(void* bullet,unsigned flags) {
    Untag(bullet);
    return originalDtor(bullet,flags);
}

std::int32_t BaseAlive(unsigned char* weapon) noexcept;

// Wanted turret yaw/pitch for a point in the vehicle frame.
void AimAngles(const float* local,float& yaw,float& pitch) noexcept {
    yaw=cfg.yawSign*std::atan2(local[0],local[2])+cfg.yawOffset;
    pitch=cfg.pitchSign*std::atan2(local[1],std::sqrt(local[0]*local[0]+local[2]*local[2]))+cfg.pitchOffset;
}

// The tracked enemy stays the target while it lives within the gun's full range (dropping it at the
// tracking-range edge made the turret flip between targets every second or two). A new one comes
// from within tracking range: any air target beats any ground one, then distance plus the turn it
// costs from where the guns point now. Returns the enemy object; `world` is its aim point, always
// taken from the object's first lock point so the lead sees a steady track.
const void* PickTarget(const unsigned char* vehicle,const unsigned char* seat,const void* keep,bool& armed,float* world,float& speed) noexcept {
    armed=false;speed=0.0f;
    float range=0.0f;
    const auto holders=At<const unsigned char* const*>(seat,kSeatWeapons);
    const auto count=At<std::uint64_t>(seat,kSeatWeaponCount);
    if(count>8 || !Readable(holders,count*8))return nullptr;
    for(std::uint64_t i=0;i<count;++i) {
        if(!Readable(holders[i],kHolderWeapon+8))continue;
        const auto weapon=At<const unsigned char*>(holders[i],kHolderWeapon);
        if(!Readable(weapon,kAmmoAlive+4) || At<std::int32_t>(weapon,kLockonType)!=kOurLockonType)continue;
        armed=true;++diag.weapons;speed=At<float>(weapon,kAmmoSpeed);
        const float reach=speed*static_cast<float>(BaseAlive(const_cast<unsigned char*>(weapon)));
        if(reach>range)range=reach;
        HookSpawn(weapon);
    }
    if(!armed)return nullptr;
    ScanEnemies(vehicle,range);
    const float track=cfg.trackRange*range;
    const auto axes=seat+kSeatAim+kAimAxes;
    const float yaw=At<float>(axes,kAxisAngle),pitch=At<float>(axes+kAxisStride,kAxisAngle);
    const bool aimed=std::isfinite(yaw) && std::isfinite(pitch);
    // Targets the guns cannot elevate (or depress) to are out: chasing one overhead pinned the
    // pitch at its stop while the yaw whipped around, and every round went under it.
    const float pitchMin=At<float>(axes+kAxisStride,kAxisMin)-kPitchMargin,pitchMax=At<float>(axes+kAxisStride,kAxisMax)+kPitchMargin;
    const auto reachable=[&](const float* l) noexcept {
        float wantYaw,wantPitch;AimAngles(l,wantYaw,wantPitch);
        return wantPitch>=pitchMin && wantPitch<=pitchMax;
    };
    const void* best=nullptr;float bestScore=0.0f;
    bool kept=false;
    for(int i=0;keep && i<enemyCount;++i) {
        if(enemies[i].object!=keep)continue;    // the scan already limits it to full range
        float l[3];ToLocal(vehicle,enemies[i].pos,l);
        if(reachable(l)){best=keep;kept=true;}
        break;
    }
    for(int i=0;!kept && i<enemyCount;++i) {
        float l[3];ToLocal(vehicle,enemies[i].pos,l);
        const float distance=std::sqrt(Dot(l,l));
        if(distance>track || !reachable(l))continue;
        float wantYaw,wantPitch;AimAngles(l,wantYaw,wantPitch);
        const float turn=aimed ? std::fabs(Wrap(wantYaw-yaw))+std::fabs(wantPitch-pitch) : 0.0f;
        const float score=distance+turn*cfg.slewWeight+(l[1]>cfg.airHeight ? 0.0f : 1.0e6f);
        if(!best || score<bestScore){best=enemies[i].object;bestScore=score;}
    }
    if(!best)return nullptr;
    for(int i=0;i<enemyCount;++i)
        if(enemies[i].object==best){std::memcpy(world,enemies[i].pos,sizeof(enemies[i].pos));break;}
    return best;
}

// Steer runs once per game frame, so the target's velocity is its per-call displacement
// (smoothed against bone jitter). Lead with the flight time to the lead point itself.
void Lead(const unsigned char* vehicle,Track& track,const void* target,const float* world,float* local,float speed) noexcept {
    const bool same=track.target==target && GetTickCount64()-track.at<200;
    if(!same){track.frames=0;std::memset(track.vel,0,sizeof(track.vel));}
    else {
        for(int i=0;i<3;++i) {
            const float v=world[i]-track.last[i];
            track.vel[i]=track.frames ? track.vel[i]+0.3f*(v-track.vel[i]) : v;
        }
        ++track.frames;
    }
    track.target=target;std::memcpy(track.last,world,sizeof(track.last));
    float aim[3];std::memcpy(aim,world,sizeof(aim));
    if(cfg.lead && speed>0.01f && track.frames>=2) {
        for(int pass=0;pass<2;++pass) {
            ToLocal(vehicle,aim,local);
            const float t=std::sqrt(Dot(local,local))/speed;
            for(int i=0;i<3;++i)aim[i]=world[i]+track.vel[i]*t;
        }
    }
    ToLocal(vehicle,aim,local);
}

// Turret input for one axis: feed-forward at the wanted angle's own rate plus a correction on the
// error. A pure proportional input lags a crossing target by rate/(gain x k), which put every
// round behind the target.
float AxisInput(Track& track,int a,float want,float angle,float error,bool wrap) noexcept {
    if(track.k[a]<=0.0f)track.k[a]=kTurnPerInput;
    if(track.frames>=1) {
        const float wanted=wrap ? Wrap(want-track.want[a]) : want-track.want[a];
        track.rate[a]+=0.3f*(wanted-track.rate[a]);
        const float moved=wrap ? Wrap(angle-track.axis[a]) : angle-track.axis[a];
        if(std::fabs(track.in[a])>0.15f) {
            const float k=moved/track.in[a];
            if(k>kTurnPerInputMin && k<kTurnPerInputMax)track.k[a]+=0.05f*(k-track.k[a]);
        }
    } else track.rate[a]=0.0f;
    const float ff=cfg.feedForward ? track.rate[a]/track.k[a] : 0.0f;
    const float in=Clamp(ff+error*cfg.gain,-1.0f,1.0f);
    track.want[a]=want;track.axis[a]=angle;track.in[a]=in;
    return in;
}

std::int32_t BaseAlive(unsigned char* weapon) noexcept {
    for(auto& f:fuses)if(f.weapon==weapon)return f.alive;
    for(auto& f:fuses)if(!f.weapon){f={weapon,At<std::int32_t>(weapon,kAmmoAlive)};return f.alive;}
    fuses[0]={weapon,At<std::int32_t>(weapon,kAmmoAlive)};   // full: recycle (stale vehicles)
    return fuses[0].alive;
}

// Time fuse: rounds burst after the flight time to `distance` metres; distance<0 = max range.
void SetFuses(const unsigned char* seat,float distance) noexcept {
    const auto holders=At<unsigned char* const*>(seat,kSeatWeapons);
    const auto count=At<std::uint64_t>(seat,kSeatWeaponCount);
    if(count>8 || !Readable(holders,count*8))return;
    for(std::uint64_t i=0;i<count;++i) {
        if(!Readable(holders[i],kHolderWeapon+8))continue;
        const auto weapon=At<unsigned char*>(holders[i],kHolderWeapon);
        if(!Readable(weapon,kAmmoAlive+4,true) || At<std::int32_t>(weapon,kLockonType)!=kOurLockonType)continue;
        const std::int32_t base=BaseAlive(weapon);
        const float speed=At<float>(weapon,kAmmoSpeed);
        std::int32_t alive=base;
        if(cfg.fuse && distance>=0.0f && speed>0.01f) {
            const float frames=std::ceil(distance/speed+cfg.fuseBias);
            alive=static_cast<std::int32_t>(Clamp(frames,static_cast<float>(cfg.fuseMin),static_cast<float>(base)));
        }
        Put<std::int32_t>(weapon,kAmmoAlive,alive);
    }
}

void Steer(unsigned char* vehicle) noexcept {
    diag.stop="vehicle";
    if(!Readable(vehicle,kTurn+0x10,true) || vehicle[kDead] || At<std::uint32_t>(vehicle,kSeatCount)==0)return;
    const auto seat=At<const unsigned char*>(vehicle,kSeats);
    diag.stop="seat";
    if(!Readable(seat,kSeatStride))return;
    Track& track=TrackFor(vehicle);
    const auto now=GetTickCount64();
    bool armed=false;float world[3]{},local[3]{},speed=0.0f;
    const auto target=PickTarget(vehicle,seat,track.target,armed,world,speed);
    if(!armed){diag.stop="unarmed";return;}       // a stock flak: leave it alone
    if(!target){diag.stop="no-target";SetFuses(seat,-1.0f);track.at=now;track.target=nullptr;return;}
    diag.stop="aiming";
    Lead(vehicle,track,target,world,local,speed);
    SetFuses(seat,std::sqrt(Dot(local,local)));
    track.at=now;
    const auto axes=seat+kSeatAim+kAimAxes;
    const float yaw=At<float>(axes,kAxisAngle),pitch=At<float>(axes+kAxisStride,kAxisAngle);
    if(!std::isfinite(yaw) || !std::isfinite(pitch)){diag.stop="bad-axis";return;}
    float wantYaw,wantPitch;AimAngles(local,wantYaw,wantPitch);
    wantPitch=Clamp(wantPitch,At<float>(axes+kAxisStride,kAxisMin),At<float>(axes+kAxisStride,kAxisMax));
    const bool fullCircle=At<float>(axes,kAxisMax)-At<float>(axes,kAxisMin)>=2*kPi-0.01f;
    const float yawError=fullCircle ? Wrap(wantYaw-yaw) : wantYaw-yaw;
    const float in[2]={AxisInput(track,0,wantYaw,yaw,yawError,fullCircle),AxisInput(track,1,wantPitch,pitch,wantPitch-pitch,false)};
    if(!std::isfinite(in[0]) || !std::isfinite(in[1])){diag.stop="bad-input";return;}
    Put<float>(vehicle,kTurn,in[0]);Put<float>(vehicle,kTurn+4,in[1]);
    if(cfg.debug && now-track.loggedAt>500) {
        track.loggedAt=now;
        Log("AIM fuse=%dm v=%p t=%p local=(%.1f,%.1f,%.1f) yaw=%.3f->%.3f pitch=%.3f->%.3f in=(%.2f,%.2f) rate=(%.2f,%.2f)/s k=(%.2f,%.2f)/s speed=%.0fm/s",
            static_cast<int>(std::sqrt(Dot(local,local))),vehicle,target,local[0],local[1],local[2],yaw,wantYaw,pitch,wantPitch,in[0],in[1],
            track.rate[0]*60.0f,track.rate[1]*60.0f,track.k[0]*60.0f,track.k[1]*60.0f,std::sqrt(Dot(track.vel,track.vel))*60.0f);
    }
}

void ReloadConfigIfChanged() noexcept;

void FlushDiag(const void* vehicle) noexcept {
    const auto now=GetTickCount64();
    if(!cfg.debug || now-diag.at<1000)return;
    if(diag.at)Log("DIAG v=%p calls=%u ridden=%u weapons=%u registry=%u enemies=%u rounds=%u prox=%u contact=%u nearest=%.1fm last=%s",
        vehicle,diag.calls,diag.ridden,diag.weapons,diag.registry,diag.enemies,
        diag.tagged,diag.proximity,diag.contact,diag.nearest,diag.stop?diag.stop:"-");
    diag=Diag{};diag.at=now;diag.nearest=1.0e9f;
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

// Weapon fire-start (0x690BB0) refuses to fire a lock-on weapon with an empty lock list unless
// LockonType is 0 or 5. No stock weapon uses type 4 (it auto-locks exactly like type 3), so
// "cmp eax,5 / je" becomes "cmp eax,4 / jae": type 4 = auto-lock, fire with or without a lock.
constexpr std::size_t kFireGate=0x690C2E;
constexpr unsigned char kFireGateStock[]={0x83,0xF8,0x05,0x74,0x23};
constexpr unsigned char kFireGateFree[]={0x83,0xF8,0x04,0x73,0x23};

bool PatchFireGate() noexcept {
    unsigned char* at=image+kFireGate;
    if(Matches(kFireGate,kFireGateFree,sizeof(kFireGateFree)))return true;
    if(!Matches(kFireGate,kFireGateStock,sizeof(kFireGateStock)))return false;
    DWORD old=0;
    if(!VirtualProtect(at,sizeof(kFireGateFree),PAGE_EXECUTE_READWRITE,&old))return false;
    std::memcpy(at,kFireGateFree,sizeof(kFireGateFree));
    VirtualProtect(at,sizeof(kFireGateFree),old,&old);
    FlushInstructionCache(GetCurrentProcess(),at,sizeof(kFireGateFree));
    return true;
}

// Swap one vtable entry, only if it still holds the expected function.
bool PatchVtableSlot(void** slot,void* expected,void* replacement) noexcept {
    DWORD old=0;
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    const bool ok=InterlockedCompareExchangePointer(slot,replacement,expected)==expected;
    VirtualProtect(slot,sizeof(void*),old,&old);
    return ok;
}

// The proximity fuse rides on GrenadeBullet01's update; it stays off if the layout differs.
void HookGrenade() noexcept {
    const auto vtable=reinterpret_cast<void**>(image+kGrenadeVtable);
    const unsigned char age[]={0x8B,0x8D,0xF8,0x0A,0x00,0x00,0xFF,0xC1};   // mov ecx,[rbp+AF8] / inc ecx
    const unsigned char expire[]={0x3B,0x8D,0x08,0x0A,0x00,0x00};          // cmp ecx,[rbp+A08]
    const bool layout=vtable[kGrenadeUpdateSlot]==image+kGrenadeUpdate && vtable[kGrenadeDtorSlot]==image+kGrenadeDtor
        && Matches(0x236888,age,sizeof(age)) && Matches(0x236899,expire,sizeof(expire));
    if(!layout){Log("HOOK grenade: unexpected layout, proximity fuse off");return;}
    originalUpdate=reinterpret_cast<UpdateFn>(image+kGrenadeUpdate);
    originalDtor=reinterpret_cast<DtorFn>(image+kGrenadeDtor);
    const bool update=PatchVtableSlot(vtable+kGrenadeUpdateSlot,image+kGrenadeUpdate,reinterpret_cast<void*>(&UpdateHook));
    const bool dtor=PatchVtableSlot(vtable+kGrenadeDtorSlot,image+kGrenadeDtor,reinterpret_cast<void*>(&DtorHook));
    proximityReady=update && dtor;
    Log("HOOK grenade update=%d dtor=%d",update,dtor);
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
    next.lead=GetPrivateProfileIntW(L"AutoTurret",L"Lead",1,iniPath)!=0;
    next.trackRange=Clamp(ReadFloat(L"TrackRange",next.trackRange),0.0f,1.0f);
    next.feedForward=GetPrivateProfileIntW(L"AutoTurret",L"FeedForward",1,iniPath)!=0;
    next.slewWeight=Clamp(ReadFloat(L"SlewWeight",next.slewWeight),0.0f,5000.0f);
    next.fuse=GetPrivateProfileIntW(L"AutoTurret",L"FuseToTarget",1,iniPath)!=0;
    next.fuseBias=ReadFloat(L"FuseBiasFrames",next.fuseBias);
    next.fuseMin=GetPrivateProfileIntW(L"AutoTurret",L"FuseMinFrames",next.fuseMin,iniPath);
    next.proximity=ReadFloat(L"ProximityRadius",next.proximity);
    next.contact=GetPrivateProfileIntW(L"AutoTurret",L"ContactFuse",1,iniPath)!=0;
    next.burstVisual=Clamp(ReadFloat(L"BurstVisualScale",next.burstVisual),0.2f,10.0f);
    cfg=next;
    Log("CONFIG enabled=%d debug=%d gain=%.2f yaw=%.0f%+.3f pitch=%.0f%+.3f pivot=%.1f air=%.1f lead=%d track=%.2f ff=%d slew=%.0f fuse=%d%+.1f min=%d proximity=%.1f contact=%d",
        cfg.enabled,cfg.debug,cfg.gain,cfg.yawSign,cfg.yawOffset,cfg.pitchSign,cfg.pitchOffset,cfg.pivotHeight,
        cfg.airHeight,cfg.lead,cfg.trackRange,cfg.feedForward,cfg.slewWeight,cfg.fuse,cfg.fuseBias,cfg.fuseMin,cfg.proximity,cfg.contact);
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
    const bool gate=PatchFireGate();
    Log("HOOK flak input slot=%d fire-gate(type4 free fire)=%d",hooked,gate);
    HookGrenade();
    return hooked;  // never unload code a patched slot points at
}

BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){autoturret::module=instance;DisableThreadLibraryCalls(instance);}
    return TRUE;
}
