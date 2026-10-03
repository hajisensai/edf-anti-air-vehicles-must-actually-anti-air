// Shared by the flak turret (plugin.cpp) and the tank gunners (gunner.cpp): the EDF.dll layout,
// the config, the enemy scan, the ballistic solve and the turret axis control.
// All addresses are RVAs into EDF.dll TimeDateStamp 0x678CCB46; see docs/re-notes.md.
#pragma once
#include <Windows.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace autoturret {
extern unsigned char* image;

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
    float dragDeadzone=0.3f;   // aim stick past this aims by hand while held (0 = never)
    DWORD dragDropMs=3000;     // the target dragged away from is not picked again for this long
    bool fuse=true;            // set each round's lifetime (its burst point) to the target's range
    float fuseBias=0.0f;       // frames added to the computed fuse
    int fuseMin=4;             // never burst closer than this many frames
    float proximity=5.0f;      // burst when a round passes this close to a target (0 = off); keep below AmmoExplosion
    bool contact=true;         // burst the moment a round sticks to something or stops
    float burstVisual=2.5f;    // burst effect size as a multiple of the stock one (AmmoExplosion/5)
    // Tank gunners (gunner.cpp): the side guns of the Titan and the Ranger's gunner-seat tanks
    bool gunnerAi=true;        // an empty gunner seat aims and fires its gun by itself
    bool gunnerAssist=true;    // a player in a gunner seat gets the auto-aim (keeps the trigger)
    float gunnerRange=300.0f;  // metres; never farther than the gun's own range
    float gunnerCone=0.02f;    // rad; the AI fires once the barrel is this close to the aim (or the target's size)
    float gunnerMinDistance=15.0f;  // metres from the muzzle; the AI holds fire on anything closer
    float gunnerYawSign=1.0f;  // side-gun axis angle change per radian of geometric turn
    float gunnerPitchSign=-1.0f;
};
extern Config cfg;

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
// Weapon: lock-on profile (LockonType 4 marks our guns)
// LockonTargetType 1 marks a ground-attack gun: our guns never lock (LockonRange 0), and the only
// stock reader (0x696792) just picks the lock class from it.
constexpr std::size_t kLockonType=0x6B0,kLockonTargetType=0x6B4;
constexpr std::int32_t kOurLockonType=4,kGroundTargetType=1;
// Weapon ammo parameters, copied into each round when it is fired; speed is metres per frame
constexpr std::size_t kAmmoSpeed=0x894,kAmmoAlive=0x898,kAmmoDamage=0x89C,kAmmoExplosion=0x8B0,kAmmoGravity=0x8E0;
// The round factory AmmoClass resolved to (0x68D53A); fire (0x6970A5) spawns rounds through it.
// Only GrenadeBullet01 rounds can be fused, so only guns with its factory get a time fuse.
constexpr std::size_t kAmmoFactory=0x7F8;
constexpr unsigned kGrenadeFactoryVtable=0x17A1688;
// World gravity: *(global)+0x68 is the physics world; its object at +0x20 returns the gravity
// vector (m/s^2) from virtual slot 0. The game's own vehicle aim (0x622706) reads it this way and
// drops a round by AmmoGravityFactor x gravity / 3600 metres per frame^2 (0x622B65).
constexpr std::size_t kWorld=0x20B2958,kWorldPhysics=0x68,kPhysicsGravity=0x20;
constexpr float kFramesPerSecondSq=3600.0f;
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

// The turret axes turn at (input x k) rad per frame; measured from the log at ~1.1 rad/s for a
// full input on both axes, and refined online from how far each axis actually moved.
constexpr float kPitchMargin=0.03f;   // rad past a pitch stop still counted as reachable
constexpr float kTurnPerInput=1.1f/60.0f,kTurnPerInputMin=0.2f/60.0f,kTurnPerInputMax=6.0f/60.0f;

// Lock points of every enemy within gun range, sampled in the vehicle input phase that runs
// before the bullet update phase of the same frame. One enemy can own several entries.
struct Enemy { const void* object; float pos[3]; float origin[3]; };
constexpr ULONGLONG kEnemyMs=150;
extern Enemy enemies[kMaxEnemies];
extern int enemyCount;

struct Track {
    const void* vehicle;   // the key: a flak vehicle, or a tank gunner seat
    const void* target;
    float last[3];
    float vel[3];          // smoothed target velocity, metres per frame
    int frames;            // consecutive frames on this target
    float want[2];         // last wanted yaw/pitch
    float rate[2];         // smoothed change of the wanted angles, rad per frame
    float axis[2];         // last yaw/pitch angle
    float in[2];           // last input written
    float k[2];            // learned rad per frame per unit input
    bool dragging;         // the rider is aiming by hand
    const void* dropped;   // target dragged away from
    ULONGLONG droppedUntil;
    ULONGLONG at;          // last update tick
    ULONGLONG loggedAt;
    // Tank gunners: axis angle and barrel angle last frame, and the learned sign between them
    float geoAxis[2];
    float geo[2];
    float geoSign[2];      // 0 = not yet set
    bool geoValid;
};

// A gun's round as the aim sees it: muzzle speed (m/frame), the drop it picks up along the
// vehicle's down axis (m/frame^2), and whether the gun hunts ground targets first.
struct Shot { float speed; float drop; bool ground; };

template<class T> T At(const void* base,std::size_t offset) noexcept {
    T value;std::memcpy(&value,static_cast<const unsigned char*>(base)+offset,sizeof(T));return value;
}
template<class T> void Put(void* base,std::size_t offset,T value) noexcept {
    std::memcpy(static_cast<unsigned char*>(base)+offset,&value,sizeof(T));
}
inline float Dot(const float* a,const float* b) noexcept { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline float Clamp(float v,float lo,float hi) noexcept { return v<lo?lo:(v>hi?hi:v); }
inline float Wrap(float a) noexcept {
    while(a>kPi)a-=2*kPi;
    while(a<-kPi)a+=2*kPi;
    return a;
}

void Log(const char* format,...) noexcept;
Track& TrackFor(const void* key) noexcept;
bool Finite(const unsigned char* base,std::size_t offset,float* out) noexcept;
void ScanEnemies(const unsigned char* vehicle,float range) noexcept;
float Down(const unsigned char* vehicle) noexcept;
bool Ballistic(const float* local,const Shot& shot,float& elevation,float& time) noexcept;
float AxisInput(Track& track,int a,float want,float angle,float error,bool wrap) noexcept;
bool Matches(std::size_t rva,const unsigned char* bytes,std::size_t size) noexcept;
bool PatchVtableSlot(void** slot,void* expected,void* replacement) noexcept;
void ReloadConfigIfChanged() noexcept;

// gunner.cpp: hooks the Titan's and the gunner-seat tanks' input; false when it left them stock
bool HookGunners() noexcept;
}  // namespace autoturret
