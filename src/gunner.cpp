// Tank gunners: the side guns of the Titan (Vehicle404_Tank) and of the Ranger's gunner-seat tanks
// (Vehicle403_Tank) aim themselves, and an unmanned side gun also fires.
//
// Both tanks have three seats: the driver (main cannon) and the left/right gunners, each driving
// its own gun through the same VehicleWeaponAim the flak uses. Their per-frame input (vtable slot
// 55) writes the turn input of seat i to vehicle+0x2AA0+i*0x10 from that seat's stick and pulls
// trigger i (vehicle+0x638, stride 0x48) on its fire button; slot 4 then applies every seat's turn
// input unconditionally. Hooking slot 55 and rewriting seats 1 and 2 after the stock code aims them.
//
// A seat's rider is the GameObjectBase at seat+0x260 (weak_ptr control block at +0x268). A player
// rider is one driven by a pad: +0x340 (pad) set and +0x354 (player-controlled) on, the same test
// the human (0x572EFF) and vehicle (0x673AC2) code use before reading a pad. Anything else in a
// seat (an NPC soldier, the DummyVehicleRider the game seats in NPC-crewed vehicles) is not.
//   player in the seat  -> auto-aim only (GunnerAssist); the trigger stays theirs, the stick takes over
//   no player, crewed   -> aim and fire (GunnerAI): player-driven or NPC-driven tanks alike
//   nobody aboard       -> left alone
//
// The aim is closed on the real barrel: every frame the muzzle frame is rebuilt from its bone's
// world rows and the muzzle's local matrix (the transform fire-time 0x6969A0 runs), so the aim does
// not depend on where the gun's pivot sits or how its axes map, except for one sign per axis
// (axis angle vs geometric angle), which is learned from how the barrel actually moves.
// All addresses are RVAs into EDF.dll TimeDateStamp 0x678CCB46; see docs/re-notes.md.
#include "memory.h"
#include "turret.h"

namespace autoturret {
namespace {
constexpr unsigned kTank403Vtable=0x17D8FA0,kTank403Input=0x5FEBE0;   // Vehicle403_Tank, slot 55
constexpr unsigned kTank404Vtable=0x17D9458,kTank404Input=0x5FFC50;   // Vehicle404_Tank (Titan)
constexpr unsigned kPullTrigger=0x62C000;   // (trigger): fire this frame if the weapon is alive
// Triggers: vehicle+0x638 array, +0x648 count, stride 0x48; +8 the weapon's weak_ptr control block,
// +0x10 the weapon. Trigger i belongs to seat i (the Titan's 3..5 are the seats' secondary weapons).
constexpr std::size_t kTriggers=0x638,kTriggerCount=0x648,kTriggerStride=0x48,kTriggerCtrl=0x8,kTriggerWeapon=0x10;
constexpr std::size_t kRider=0x260,kRiderCtrl=0x268,kRiderPad=0x340,kRiderPlayer=0x354;
// Weapon muzzles: array at +0x1D0, count at +0x1E0, stride 0xF0. Muzzle +0 is its bone (world rows
// right/up/forward/position at +0xB0..+0xEF, updated every frame), +0x10 its local 4x4 matrix,
// +0xE0 how fire orients it (0x696B70): mode 0 takes the weapon's own world rows (weapon+0x150,
// copied from its aim bone each frame by 0x633DD0), mode 1 the local matrix times the bone.
// The round leaves along row 2 (+0x70 of the built matrix, 0x69168B).
constexpr std::size_t kMuzzles=0x1D0,kMuzzleCount=0x1E0,kMuzzleStride=0xF0,kMuzzleLocal=0x10,kBoneRows=0xB0;
constexpr std::size_t kMuzzleMode=0xE0,kWeaponMatrix=0x150;
constexpr std::int32_t kModeWeaponRows=0;
constexpr std::size_t kTurnStride=0x10;
constexpr unsigned kGunnerSeats=3;          // driver + two gunners
constexpr float kMuzzleReach=30.0f;         // a muzzle farther than this from the vehicle is garbage
// The side guns turn far slower than the flak turret (~0.3 rad/s against ~1.1), so a fixed
// stick-per-radian gain leaves them a second behind a tank that is driving. Their gain comes from
// the learned turn rate instead: an error is asked to close in this many frames.
constexpr float kSettleFrames=6.0f;
constexpr float kHoldCone=2.0f;             // a firing gun keeps firing until it is this many cones off
constexpr std::size_t kWeaponFire=0x139;    // the trigger 0x62C000 sets; the weapon update reads and clears it

using InputFn=void(__fastcall*)(void*,std::uintptr_t);
using TriggerFn=void(__fastcall*)(void*);
InputFn original403=nullptr,original404=nullptr;

enum class Crew { none, ai, player };

// A side gun as the aim sees it this frame: its trigger, round and muzzle in the world.
struct Gun {
    void* trigger;
    const unsigned char* weapon;
    float pos[3];      // muzzle position, world
    float dir[3];      // barrel direction, world, unit
    Shot shot;
    float range;       // metres
    float blast;       // AmmoExplosion radius, metres (0 = solid round)
};

// The vehicle's rows: right, up, forward, position.
const float* Frame(const unsigned char* vehicle) noexcept { return reinterpret_cast<const float*>(vehicle+kMatrix); }

void ToFrame(const float* m,const float* v,float* out) noexcept {
    out[0]=Dot(v,m);out[1]=Dot(v,m+4);out[2]=Dot(v,m+8);
}

// Geometric yaw (around the vehicle's up, toward its right) and elevation of a vehicle-frame vector.
void Angles(const float* local,float* geo) noexcept {
    geo[0]=std::atan2(local[0],local[2]);
    geo[1]=std::atan2(local[1],std::sqrt(local[0]*local[0]+local[2]*local[2]));
}

bool AllFinite(const float* v,int n) noexcept {
    for(int i=0;i<n;++i)if(!std::isfinite(v[i]))return false;
    return true;
}

Crew SeatCrew(const unsigned char* seat) noexcept {
    const auto ctrl=At<const unsigned char*>(seat,kRiderCtrl);
    if(!ctrl || !Readable(ctrl,0x10) || At<std::int32_t>(ctrl,8)==0)return Crew::none;
    const auto rider=At<const unsigned char*>(seat,kRider);
    if(!Readable(rider,kRiderPlayer+1))return Crew::ai;
    return rider[kRiderPlayer] && At<const void*>(rider,kRiderPad) ? Crew::player : Crew::ai;
}

// One muzzle's world position and direction as 0x6969A0 builds them for a shot: the position is
// row 3 of local x bone, the direction row 2 of the matrix its mode picks.
bool MuzzleFrame(const unsigned char* weapon,const unsigned char* muzzle,float* pos,float* dir) noexcept {
    const auto bone=At<const unsigned char*>(muzzle,0);
    if(!Readable(bone,kBoneRows+0x40))return false;
    float b[4][4],l[4][4];
    std::memcpy(b,bone+kBoneRows,sizeof(b));std::memcpy(l,muzzle+kMuzzleLocal,sizeof(l));
    for(int c=0;c<3;++c) {
        dir[c]=l[2][0]*b[0][c]+l[2][1]*b[1][c]+l[2][2]*b[2][c];
        pos[c]=l[3][0]*b[0][c]+l[3][1]*b[1][c]+l[3][2]*b[2][c]+l[3][3]*b[3][c];
    }
    if(At<std::int32_t>(muzzle,kMuzzleMode)==kModeWeaponRows)std::memcpy(dir,weapon+kWeaponMatrix+0x20,12);
    const float length=std::sqrt(Dot(dir,dir));
    return AllFinite(pos,3) && std::isfinite(length) && length>0.5f && length<2.0f;
}

// The barrel: the mean of the gun's muzzles (the Titan's side cannons have two).
bool Barrel(const unsigned char* vehicle,const unsigned char* weapon,float* pos,float* dir) noexcept {
    const auto muzzles=At<const unsigned char*>(weapon,kMuzzles);
    const auto count=At<std::uint64_t>(weapon,kMuzzleCount);
    if(count==0 || count>8 || !Readable(muzzles,count*kMuzzleStride))return false;
    std::memset(pos,0,12);std::memset(dir,0,12);
    for(std::uint64_t i=0;i<count;++i) {
        float p[3],f[3];
        if(!MuzzleFrame(weapon,muzzles+i*kMuzzleStride,p,f))return false;
        for(int c=0;c<3;++c){pos[c]+=p[c];dir[c]+=f[c];}
    }
    const float length=std::sqrt(Dot(dir,dir));
    if(!(length>0.1f))return false;
    for(int c=0;c<3;++c){pos[c]/=static_cast<float>(count);dir[c]/=length;}
    const float* m=Frame(vehicle);
    const float d[3]={pos[0]-m[12],pos[1]-m[13],pos[2]-m[14]};
    return Dot(d,d)<kMuzzleReach*kMuzzleReach;
}

bool ReadGun(const unsigned char* vehicle,unsigned s,float down,Gun& gun) noexcept {
    const auto triggers=At<unsigned char*>(vehicle,kTriggers);
    if(At<std::uint64_t>(vehicle,kTriggerCount)<=s || !Readable(triggers+s*kTriggerStride,kTriggerStride))return false;
    gun.trigger=triggers+s*kTriggerStride;
    const auto ctrl=At<const unsigned char*>(gun.trigger,kTriggerCtrl);
    if(!ctrl || !Readable(ctrl,0x10) || At<std::int32_t>(ctrl,8)==0)return false;
    gun.weapon=At<const unsigned char*>(gun.trigger,kTriggerWeapon);
    if(!Readable(gun.weapon,kAmmoGravity+4) || !Readable(gun.weapon+kWeaponMatrix,0x40) || !Barrel(vehicle,gun.weapon,gun.pos,gun.dir))return false;
    gun.shot=Shot{};
    gun.shot.speed=At<float>(gun.weapon,kAmmoSpeed);
    const float gravity=At<float>(gun.weapon,kAmmoGravity);
    if(std::isfinite(gravity) && gravity>0.0f)gun.shot.drop=gravity*down/kFramesPerSecondSq;
    const float reach=gun.shot.speed*static_cast<float>(At<std::int32_t>(gun.weapon,kAmmoAlive));
    gun.range=std::isfinite(reach) && reach>0.0f ? (reach<cfg.gunnerRange ? reach : cfg.gunnerRange) : 0.0f;
    const float blast=At<float>(gun.weapon,kAmmoExplosion);
    gun.blast=std::isfinite(blast) && blast>0.0f ? blast : 0.0f;
    return gun.shot.speed>0.01f && std::isfinite(gun.shot.speed) && gun.range>0.0f;
}

// Geometric yaw/elevation the barrel needs to hit `world` from the muzzle; false if out of reach.
bool Solve(const unsigned char* vehicle,const Gun& gun,const float* world,float* want,float& time,float& distance) noexcept {
    const float d[3]={world[0]-gun.pos[0],world[1]-gun.pos[1],world[2]-gun.pos[2]};
    float local[3];ToFrame(Frame(vehicle),d,local);
    distance=std::sqrt(Dot(d,d));
    float elevation;
    if(distance>gun.range || !Ballistic(local,gun.shot,elevation,time))return false;
    want[0]=std::atan2(local[0],local[2]);want[1]=elevation;
    return true;
}

// The seat's two aim axes and where the barrel points now (geometric, vehicle frame).
struct Aim {
    float angle[2],min[2],max[2];
    float barrel[2];
    float sign[2];     // geometric angle change per unit axis angle change, +-1
};

bool ReadAim(const unsigned char* vehicle,const unsigned char* seat,const Gun& gun,Track& track,Aim& aim) noexcept {
    const auto axes=seat+kSeatAim+kAimAxes;
    for(int a=0;a<2;++a) {
        aim.angle[a]=At<float>(axes+a*kAxisStride,kAxisAngle);
        aim.min[a]=At<float>(axes+a*kAxisStride,kAxisMin);
        aim.max[a]=At<float>(axes+a*kAxisStride,kAxisMax);
    }
    float local[3];ToFrame(Frame(vehicle),gun.dir,local);
    Angles(local,aim.barrel);
    if(!AllFinite(aim.angle,2) || !AllFinite(aim.min,2) || !AllFinite(aim.max,2) || !AllFinite(aim.barrel,2))return false;
    // Learn each axis' sign from the barrel's own motion: the pivot geometry is the model's, and a
    // wrong guess would drive the gun into its stop and keep it there.
    const float initial[2]={cfg.gunnerYawSign,cfg.gunnerPitchSign};
    for(int a=0;a<2;++a) {
        if(track.geoSign[a]==0.0f)track.geoSign[a]=initial[a]>=0.0f ? 1.0f : -1.0f;
        const float moved=aim.angle[a]-track.geoAxis[a];
        const float turned=a==0 ? Wrap(aim.barrel[a]-track.geo[a]) : aim.barrel[a]-track.geo[a];
        const float ratio=std::fabs(moved)>0.002f ? turned/moved : 0.0f;
        if(track.geoValid && std::fabs(ratio)>0.3f && std::fabs(ratio)<3.0f)
            track.geoSign[a]+=0.2f*((ratio>0.0f ? 1.0f : -1.0f)-track.geoSign[a]);
        track.geoAxis[a]=aim.angle[a];track.geo[a]=aim.barrel[a];
        aim.sign[a]=track.geoSign[a]>=0.0f ? 1.0f : -1.0f;
    }
    track.geoValid=true;
    return true;
}

// Geometric error to `want`, and the axis angles that close it; false when an axis can't get there.
bool AxisTargets(const Aim& aim,const float* want,float* error,float* axis) noexcept {
    error[0]=Wrap(want[0]-aim.barrel[0]);error[1]=want[1]-aim.barrel[1];
    for(int a=0;a<2;++a) {
        axis[a]=aim.angle[a]+aim.sign[a]*error[a];
        if(axis[a]<aim.min[a]-kPitchMargin || axis[a]>aim.max[a]+kPitchMargin)return false;
    }
    return true;
}

// Keep the current target while the gun can still reach it, else the cheapest one in reach:
// distance plus the turn it costs, weighted as the flak weighs it.
const void* PickGunTarget(const unsigned char* vehicle,const Gun& gun,const Aim& aim,const void* keep,const void* dropped,float* world) noexcept {
    const void* best=nullptr;float bestScore=0.0f;int bestAt=-1;
    for(int i=0;i<enemyCount;++i) {
        const Enemy& e=enemies[i];
        if(e.object==dropped)continue;
        float want[2],error[2],axis[2],time,distance;
        if(!Solve(vehicle,gun,e.pos,want,time,distance) || !AxisTargets(aim,want,error,axis))continue;
        if(e.object==keep){best=keep;bestAt=i;break;}
        const float score=distance+(std::fabs(error[0])+std::fabs(error[1]))*cfg.slewWeight;
        if(!best || score<bestScore){best=e.object;bestScore=score;bestAt=i;}
    }
    if(!best)return nullptr;
    for(int i=0;i<=bestAt;++i)   // the object's first lock point, for a steady track
        if(enemies[i].object==best){std::memcpy(world,enemies[i].pos,sizeof(enemies[i].pos));break;}
    return best;
}

// Lead the target by the round's flight time, with its velocity smoothed per frame as the flak does.
void LeadGun(const unsigned char* vehicle,const Gun& gun,Track& track,const void* target,const float* world,float* aim) noexcept {
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
    std::memcpy(aim,world,12);
    if(!cfg.lead || track.frames<2)return;
    for(int pass=0;pass<2;++pass) {
        float want[2],time,distance;
        if(!Solve(vehicle,gun,aim,want,time,distance))return;
        for(int i=0;i<3;++i)aim[i]=world[i]+track.vel[i]*time;
    }
}

// The AI holds fire until the barrel is on the aim point (or within the target's blast size)
// and never fires on something so close the round would hit the tank or its own blast would.
// Once firing it holds the trigger through the wobble of a driving tank (`widen` cones), since the
// weapon reads the trigger as held only while it is pulled every frame.
bool OnTarget(const Gun& gun,const float* error,float distance,float widen) noexcept {
    const float size=gun.blast>1.5f ? gun.blast : 1.5f;
    const float cone=widen*std::fmax(cfg.gunnerCone,std::atan(size/std::fmax(distance,1.0f)));
    const float closest=std::fmax(cfg.gunnerMinDistance,1.5f*gun.blast);
    return std::fabs(error[0])<cone && std::fabs(error[1])<cone && distance>=closest;
}

void SteerSeat(unsigned char* vehicle,unsigned s,Crew crew,float down) noexcept {
    const auto seat=At<const unsigned char*>(vehicle,kSeats)+s*kSeatStride;
    Gun gun{};Aim aim{};
    if(!ReadGun(vehicle,s,down,gun))return;
    Track& track=TrackFor(seat);
    const auto now=GetTickCount64();
    if(!ReadAim(vehicle,seat,gun,track,aim))return;
    // A player holding the stick aims by hand; letting go hands the gun back, never to the target
    // it was dragged away from.
    const float stick[2]={At<float>(seat,kStick),At<float>(seat,kStick+4)};
    const bool drag=crew==Crew::player && cfg.dragDeadzone>0.0f
        && (std::fabs(stick[0])>cfg.dragDeadzone || std::fabs(stick[1])>cfg.dragDeadzone);
    if(drag && !track.dragging && track.target){track.dropped=track.target;track.droppedUntil=now+cfg.dragDropMs;}
    track.dragging=drag;
    if(drag){track.target=nullptr;track.at=now;return;}
    const void* dropped=now<track.droppedUntil ? track.dropped : nullptr;
    float world[3],aimAt[3];
    const auto target=PickGunTarget(vehicle,gun,aim,track.target,dropped,world);
    if(!target){track.target=nullptr;track.firing=false;track.at=now;return;}
    LeadGun(vehicle,gun,track,target,world,aimAt);
    track.at=now;
    float want[2],error[2],axis[2],time,distance;
    if(!Solve(vehicle,gun,aimAt,want,time,distance) || !AxisTargets(aim,want,error,axis)){track.firing=false;return;}
    for(int a=0;a<2;++a)axis[a]=Clamp(axis[a],aim.min[a],aim.max[a]);
    float in[2];
    for(int a=0;a<2;++a) {
        const float k=track.k[a]>0.0f ? track.k[a] : kTurnPerInput;
        in[a]=AxisInput(track,a,axis[a],aim.angle[a],axis[a]-aim.angle[a],false,1.0f/(k*kSettleFrames));
    }
    if(!AllFinite(in,2))return;
    Put<float>(vehicle,kTurn+s*kTurnStride,in[0]);Put<float>(vehicle,kTurn+s*kTurnStride+4,in[1]);
    const bool fire=crew!=Crew::player && OnTarget(gun,error,distance,track.firing ? kHoldCone : 1.0f);
    if(fire) {
        // A pull the weapon has not read by the next frame means this gun is not being updated.
        if(track.firing && gun.weapon[kWeaponFire])++track.stale;
        ++track.pulls;
        reinterpret_cast<TriggerFn>(image+kPullTrigger)(gun.trigger);
    }
    track.firing=fire;
    if(cfg.debug && now-track.loggedAt>500) {
        track.loggedAt=now;
        Log("GUNNER v=%p seat=%u %s t=%p dist=%.0f flight=%.0ff barrel=(%.3f,%.3f) want=(%.3f,%.3f) axis=(%.3f,%.3f)->(%.3f,%.3f) sign=(%+.0f,%+.0f) k=(%.4f,%.4f) in=(%.2f,%.2f) fire=%d pulls=%u stale=%u",
            vehicle,s,crew==Crew::player?"player":"ai",target,distance,time,aim.barrel[0],aim.barrel[1],want[0],want[1],
            aim.angle[0],aim.angle[1],axis[0],axis[1],aim.sign[0],aim.sign[1],track.k[0],track.k[1],in[0],in[1],fire,track.pulls,track.stale);
        track.pulls=0;track.stale=0;
    }
}

void Gunners(unsigned char* vehicle) noexcept {
    if(!Readable(vehicle,kTurn+kGunnerSeats*kTurnStride,true) || vehicle[kDead])return;
    if(At<std::uint32_t>(vehicle,kSeatCount)<kGunnerSeats)return;
    const auto seats=At<const unsigned char*>(vehicle,kSeats);
    if(!Readable(seats,kGunnerSeats*kSeatStride))return;
    Crew crew[kGunnerSeats];
    bool crewed=false;
    for(unsigned s=0;s<kGunnerSeats;++s){crew[s]=SeatCrew(seats+s*kSeatStride);crewed=crewed || crew[s]!=Crew::none;}
    if(!crewed)return;                  // a parked tank stays quiet
    bool any=false;
    for(unsigned s=1;s<kGunnerSeats;++s)
        any=any || (crew[s]==Crew::player ? cfg.gunnerAssist : cfg.gunnerAi);
    if(!any)return;
    ScanEnemies(vehicle,cfg.gunnerRange+kMuzzleReach);
    const float down=Down(vehicle);
    for(unsigned s=1;s<kGunnerSeats;++s) {
        const bool wanted=crew[s]==Crew::player ? cfg.gunnerAssist : cfg.gunnerAi;
        if(wanted)SteerSeat(vehicle,s,crew[s]==Crew::player ? Crew::player : Crew::ai,down);
    }
}

void Run(void* vehicle) noexcept {
    ReloadConfigIfChanged();
    if(!cfg.enabled || (!cfg.gunnerAi && !cfg.gunnerAssist))return;
    __try { Gunners(static_cast<unsigned char*>(vehicle)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// The stock input runs first in both: it zeroes or fills every seat's turn input and handles the
// riders' own triggers; the gunners only overwrite seats 1 and 2 after it.
void __fastcall Hook403(void* vehicle,std::uintptr_t hasInput) { original403(vehicle,hasInput);Run(vehicle); }
void __fastcall Hook404(void* vehicle,std::uintptr_t hasInput) { original404(vehicle,hasInput);Run(vehicle); }

struct Signature { std::size_t rva; unsigned char bytes[27]; std::size_t size; };

// The code the gunners depend on; any mismatch leaves the tanks stock.
const Signature kSignatures[]={
    {kTank403Input,{0x48,0x89,0x5C,0x24,0x20,0x55,0x48,0x83,0xEC,0x30,0x0F,0xB6,0xDA},13},
    {kTank404Input,{0x48,0x89,0x5C,0x24,0x20,0x55,0x48,0x83,0xEC,0x30,0x0F,0xB6,0xDA},13},
    {0x5FEC44,{0x48,0x8D,0xBD,0xA4,0x2A,0x00,0x00},7},                    // 403: lea rdi,[rbp+2AA4] (turn)
    {0x5FEC88,{0x48,0x8B,0x85,0x38,0x06,0x00,0x00},7},                    // 403: mov rax,[rbp+638] (triggers)
    {0x5FEC98,{0xF3,0x0F,0x10,0x86,0xD0,0x02,0x00,0x00},8},               // 403: movss xmm0,[rsi+2D0] (stick)
    {0x5FECA5,{0x49,0x81,0xC6,0x40,0x03,0x00,0x00},7},                    // 403: add r14,340 (seat stride)
    {0x5FFCB4,{0x48,0x8D,0xB5,0xA4,0x2A,0x00,0x00},7},                    // 404: lea rsi,[rbp+2AA4]
    {0x5FFCF8,{0x48,0x8B,0x85,0x38,0x06,0x00,0x00},7},                    // 404: mov rax,[rbp+638]
    {0x5FFD3A,{0x49,0x81,0xC6,0x40,0x03,0x00,0x00},7},                    // 404: add r14,340
    {0x5FEF19,{0x4B,0x8B,0x84,0x34,0xE0,0x00,0x00,0x00},8},               // 403 slot 4: seat+E0 aim
    {0x5FEF21,{0x48,0x8D,0x95,0xAA,0x02,0x00,0x00},7},                    // 403 slot 4: lea rdx,[rbp+2AA]
    {0x600049,{0x4B,0x8B,0x84,0x34,0xE0,0x00,0x00,0x00},8},               // 404 slot 4: seat+E0 aim
    {0x600051,{0x48,0x8D,0x95,0xAA,0x02,0x00,0x00},7},                    // 404 slot 4: lea rdx,[rbp+2AA]
    {kPullTrigger,{0x48,0x8B,0x41,0x08,0x48,0x85,0xC0,0x74,0x11,0x83,0x78,0x08,0x00,0x74,0x0B,
                   0x48,0x8B,0x41,0x10,0xC6,0x80,0x39,0x01,0x00,0x00,0x01,0xC3},27},
    {0x6330A9,{0x48,0x8B,0x86,0x48,0x06,0x00,0x00},7},                    // mov rax,[rsi+648] (trigger count)
    {0x63407F,{0x49,0x89,0xAE,0x60,0x02,0x00,0x00},7},                    // mov [r14+260],rbp (seat rider)
    {0x572E49,{0x48,0x83,0xB9,0x40,0x03,0x00,0x00,0x00},8},               // cmp qword [rcx+340],0 (pad)
    {0x572EFF,{0x44,0x38,0xAE,0x54,0x03,0x00,0x00},7},                    // cmp [rsi+354],r13b (player)
    {0x6969AB,{0x0F,0x10,0x41,0x10},4},                                   // muzzle local at +0x10
    {0x696B70,{0x8B,0x89,0xE0,0x00,0x00,0x00,0x85,0xC9,0x0F,0x84,0xD9,0x01,0x00,0x00,0x83,0xE9,0x01},17},   // mode 0 / 1 dispatch
    {0x696DB3,{0x41,0x0F,0x10,0x00},4},                                   // mode 0: rows from the weapon matrix
    {0x6904BE,{0x48,0x8D,0x93,0x50,0x01,0x00,0x00},7},                    // ... passed as weapon+0x150
    {0x633E03,{0x0F,0x29,0x82,0x50,0x01,0x00,0x00},7},                    // weapon+0x150 <- aim bone rows
    {0x69168B,{0x44,0x0F,0x10,0x4B,0x70},5},                              // shot direction: row 2 (+0x70)
    {0x696A03,{0x48,0x8B,0x11},3},                                        // muzzle bone at +0
    {0x696A28,{0x0F,0x10,0xAA,0xC0,0x00,0x00,0x00},7},                    // bone rows +B0..+E0
    {0x696A2F,{0x0F,0x10,0xB2,0xB0,0x00,0x00,0x00},7},
    {0x696A36,{0x0F,0x10,0xBA,0xD0,0x00,0x00,0x00},7},
    {0x696A3D,{0x44,0x0F,0x10,0x82,0xE0,0x00,0x00,0x00},8},
    {0x697019,{0x48,0xF7,0xB1,0xE0,0x01,0x00,0x00},7},                    // fire: muzzle count +1E0
    {0x697022,{0x48,0x69,0xD8,0xF0,0x00,0x00,0x00,0x48,0x03,0x99,0xD0,0x01,0x00,0x00},14},   // stride F0, array +1D0
};

bool CheckGunnerProfile() noexcept {
    __try {
        for(const auto& s:kSignatures)if(!Matches(s.rva,s.bytes,s.size))return false;
        return reinterpret_cast<void**>(image+kTank403Vtable)[kInputSlot]==image+kTank403Input
            && reinterpret_cast<void**>(image+kTank404Vtable)[kInputSlot]==image+kTank404Input;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}  // namespace

bool HookGunners() noexcept {
    if(!image || !CheckGunnerProfile()){Log("HOOK gunners: unexpected layout, tank gunners off");return false;}
    original403=reinterpret_cast<InputFn>(image+kTank403Input);
    original404=reinterpret_cast<InputFn>(image+kTank404Input);
    const bool tank=PatchVtableSlot(reinterpret_cast<void**>(image+kTank403Vtable)+kInputSlot,image+kTank403Input,reinterpret_cast<void*>(&Hook403));
    const bool titan=PatchVtableSlot(reinterpret_cast<void**>(image+kTank404Vtable)+kInputSlot,image+kTank404Input,reinterpret_cast<void*>(&Hook404));
    Log("HOOK gunners tank403=%d titan404=%d",tank,titan);
    return tank || titan;
}
}  // namespace autoturret
