# EDF6：防空车一定要能防空

[English](README.md)

原版 KG6 克卜勒是 EDF6 的防空车，却打着又慢又不会爆炸的实心弹，对空中目标大多打空，耐久还不到同级坦克的一半。
这个 mod 让它真正能防空，并把 DLC 的 KG7 玻尔斯改成会自己瞄准的对地榴弹车。

由两部分组成：

- **EDF6AutoTurret.dll**：[EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 插件。炮塔自己瞄准：
  直接从游戏的敌人列表里选目标，计算提前量，按炮弹的抛物线解算仰角，并用前馈控制炮塔，炮弹不再拖在横穿目标的身后。
  高射炮弹带定时引信（目标距离处空爆）、近炸引信和触发引信。按住瞄准摇杆可以手动瞄准，松开后炮塔立即接管。
- **武器数据**：直接覆盖原版载具自己的文件，不新增武器行。`WEAPONTEXT` 里这几辆载具的说明会改成新数值；只改它们自己的行，并叠加在 `Mods` 里已有的表上，改表的 mod 的内容会保留。

## 改了什么

| 载具 | 改动 |
|---|---|
| KG6 克卜勒、E、F、YE、YF | 改为高射炮：爆炸弹（爆炸半径 8m），近炸／定时／触发引信，射程 480m。射速减半、单发伤害 ×2（账面 DPS 不变，同屏爆炸减半）。耐久 ×2。炮塔转速用 DLC YF-HV 的。优先打空中目标。 |
| KG6 克卜勒 YF-HV（DLC） | 只加自瞄；保留它的高速实心弹、耐久和炮塔。 |
| KG7 玻尔斯、玻尔斯 B（DLC） | 对地模式自瞄：优先打地面目标，按抛物线瞄准，保持原版触地爆炸。耐久 ×2，爆炸半径 4m → 6m，爆炸可以破坏建筑。 |

原因：原版克卜勒的 DPS 只有同级坦克、直升机的 1/3～1/2，耐久不到一半，射程还是同期最短。玻尔斯的 DPS 已经高于同级的霸里亚斯 TZ4，但耐久远不到对方的一半。

## 安装

需要 Steam 版 EDF6，并已安装 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader)。

1. 把 release（或 CI 构建产物）里的 `EDF6AutoTurret.dll` 和 `EDF6AutoTurret.ini` 放进 `<EDF6>\Mods\Plugins\`。
2. 用你自己的游戏数据生成武器文件（它们派生自游戏数据，所以不随包分发），需要 Python 3.10+，直接输出到游戏的 `Mods` 文件夹：

   ```
   set EDF6_DIR=C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6
   python tools\build.py --out "%EDF6_DIR%\Mods"
   ```

   它在 `Mods\WEAPON\` 下写这几辆载具自己的 call 和炮文件，以及 `WEAPONTEXT.*.SGO` 里它们的 8 行说明；只读取游戏的 `Root.cpk`，不修改它。请在游戏关闭时运行；装了别的会整份替换 `WEAPONTEXT` 的 mod 之后要再运行一次。`--no-text` 不动文本表。

卸载时删掉这些文件和插件即可（`WEAPONTEXT` 只有在没有别的 mod 装过时才删，否则重装那个 mod 的）。设置在 `EDF6AutoTurret.ini`，游戏运行中保存即生效；`Debug=1` 会把炮塔的行为写进 `EDF6AutoTurret.log`。

## 构建插件

需要 Visual Studio 2022 及 C++ x64 工具（自带 CMake 和 Ninja）：

```
build.cmd
```

DLL 输出到 `dist\Mods\Plugins\`。每次 push 都会由 CI 构建。

## 兼容性

针对 TimeDateStamp 为 `0x678CCB46` 的 EDF.dll。插件会校验要打补丁的代码，游戏更新后对不上就自动停用。

联机未经测试。这个 mod 不新增武器行，没装的玩家不会遇到自己没有的行；但每台机器都按自己的文件模拟载具，混装房间里克卜勒在各人眼里的表现不会一致。建议所有玩家都装。逆向笔记见 [docs/re-notes.md](docs/re-notes.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。随附：`third_party/EDFModLoader/PluginAPI.h`（MIT）和 `third_party/edf6-cpk`（来自 momotori01 的 EDF6MultiSlot 的 CPK / CRILAYLA 读取器，公有领域）。
