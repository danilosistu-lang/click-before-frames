Verification report — GD 2.2081 / Geode SDK 5.x
This document records what was actually checked, how, and what had to be corrected in the
original specification. Nothing in `src/main.cpp` is based on recollection of 2.204/2.206
APIs.
---
1. Methodology
Step	What was done
Bindings	Cloned `geode-sdk/bindings`, sparse-checked out `bindings/2.2081` (the 2.2081 Broma: `GeometryDash.bro`, `Cocos2d.bro`, `Extras.bro`, `inline/*.cpp`) and `bindings/include`.
Codegen	Downloaded `geode-codegen-linux` (tag `codegen`) from the bindings releases and ran `codegen Win64 bindings/2.2081 <out>` — the same invocation the bindings `CMakeLists.txt` uses (`Codegen <platform> <bindings path> <out path>`) — producing 1580 real 2.2081 header files (`Geode/binding/PlayLayer.hpp`, …). Every signature below is quoted from those files.
Loader	Cloned `geode-sdk/geode` at v5.11.2 (the release that still primarily targets 2.2081) and read `loader/include` + `loader/src` directly for settings parsing, keybinds and the `Ref`/`OverlayManager`/`listenFor*` APIs.
Physics	Read the 2.2081 bindings for the step functions and `PlayerButtonCommand`, plus public documentation on the 240 TPS lock and the CBS/CBF history.
A full compile was attempted but is not possible in this sandbox: Geode's public
headers pull `asp`, `arc`, `fmt`, `matjson` and the tulip hook headers, which Geode fetches
at CMake-configure time via CPM, and the Cocos2d headers ship MSVC-only platform
`compat/stdint.h`. Everything that can be checked without a Geode SDK + GD toolchain was
checked; the remaining risk is behavioural, not API-level.
---
2. Signature verification (2.2081, Windows addresses)
2.1 `PlayLayer`
Symbol	Verified declaration	Address
`init`	`bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects)`	win `0x3a5b00`
`update(float)`	does not exist on `PlayLayer`	—
`postUpdate`	`virtual void postUpdate(float dt)`	win `0x3b4ba0`
`resetLevel`	`virtual void resetLevel()`	win `0x3b8eb0`
`createCheckpoint`	`CheckpointObject* createCheckpoint()`	win `0x3b4fc0`
`loadFromCheckpoint`	`void loadFromCheckpoint(CheckpointObject* object)`	win `0x3b7640`
`storeCheckpoint`	`void storeCheckpoint(CheckpointObject* checkpoint)`	win `0x3b74a0`
`getLastCheckpoint`	`CheckpointObject* getLastCheckpoint()`	inline
`get`	`static PlayLayer* get()`	—
The `init` signature keeps the classic three parameters in 2.2081 — the spec had this one
right.
2.2 The physics step
`PlayLayer` inherits the step function; it does not override it:
Symbol	Verified declaration	Address
`GJBaseGameLayer::update`	`virtual void update(float dt)`	win `0x237850`
`GJBaseGameLayer::handleButton`	`void handleButton(bool down, int button, bool isPlayer1)`	win `0x2338e0`
`GJBaseGameLayer::queueButton`	`void queueButton(int button, bool push, bool isPlayer2, double timestamp)`	inline
`PlayLayer::updateVisibility`	`virtual void updateVisibility(float dt)`	win `0x3af3a0`
`PlayLayer::postUpdate`	`virtual void postUpdate(float dt)`	win `0x3b4ba0`
A scan of the whole generated `GeometryDash.bro` for `virtual void update(float dt)`
confirms `update` is declared on `GJBaseGameLayer`, `PlayerObject`, `GameObject`,
`GJEffectManager`-adjacent layers, etc. — never on `PlayLayer`.
2.3 `PlayerObject`
Symbol	Verified declaration	Address
`pushButton`	`bool` `pushButton(PlayerButton button)`	win `0x397f40`
`releaseButton`	`bool` `releaseButton(PlayerButton button)`	win `0x3981d0`
`buttonDown`	`bool buttonDown(PlayerButton button)`	inline
`switchedDirTo`	`bool switchedDirTo(PlayerButton button)`	win `0x397e30`
`update`	`virtual void update(float dt)`	win `0x388d80`
`PlayerButton` is an `enum class` (not a plain enum, not `enumKeyCodes`):
`Jump = 1`, `Left = 2`, `Right = 3`.
2.4 Geode v5 API surface used
Symbol	Verified declaration
`Mod::get()` / `getSettingValue<T>` / `setSettingValue<T>`	`loader/include/Geode/loader/Mod.hpp`
`listenForSettingChanges<T>(key, cb, mod = getMod())`	`SettingV3.hpp:845`
`listenForKeybindSettingPresses(key, cb, mod)` — callback receives `(Keybind const&, bool down, bool repeat, double timestamp)`	`SettingV3.hpp:878-882`
`Keybind` type; `KeyboardModifier` with implicit `operator bool`	`utils/Keyboard.hpp`
`geode::utils::getInputTimestamp()` → `double` seconds	`utils/general.hpp:302`
`geode::Ref<T>` (`data()`, implicit `operator T*`, `operator=(T*)`)	`utils/cocos.hpp:250+`
`OverlayManager::get()`	`ui/OverlayManager.hpp`
`CCLabelBMFont::create(const char*, const char*)`, `setString(const char*)`, `setAnchorPoint(const CCPoint&)`	cocos headers
`CCDirector::get()->getWinSize()`	cocos `CCDirector.h:143`
`$on_mod(Loaded)`	`loader/ModEvent.hpp:37`
---
3. Spec defects found and fixed
Each of these would have either failed to compile, failed to load, or silently done
nothing.
3.1 Mod ID is rejected by Geode → mod would never load
`ModMetadata::validateID` in `loader/src/loader/ModMetadataImpl.cpp:878`:
```cpp
// Only one dot permitted
for (auto const& c : id) {
    if (!( ('a' <= c && c <= 'z') || ('0' <= c && c <= '9') ||
           (c == '-' || c == '_') || (c == '.' && !foundDot) )) return false;
    ...
```
`com.username.clickbeforeframes` has two dots, so the metadata fails validation and the
mod is refused. Fixed to `username.clickbeforeframes` (and therefore
`username.clickbeforeframes.geode`). If a reverse-domain ID is mandatory for your
distribution, Geode's loader has to be patched — it is not a `mod.json` detail.
3.2 `"geode": "5.0.0"` is the wrong format
The `geode` key is a version requirement, not a version. Fixed to `">=v5.0.0"`.
3.3 The keybind `default` format does not exist in Geode 5
The spec used:
```json
"default": { "key": "cocos2d::enumKeyCodes::KEY_F6" }
```
`KeybindSettingV3::parseDefaultValue` (`SettingV3.cpp:1221`) accepts only a string or an
array of strings, parsed by `Keybind::fromString`, whose key-name table contains `"F6"`.
An object default silently produces an empty default bind. Fixed to `"default": "F6"`.
Related: `KeybindSettingV3::parse` calls `root.checkUnknownKeys()`, so any invented keys in
a setting object cause a parse error — the setting objects here only use keys the v5 parser
actually reads (`type`, `name`, `description`, `default`, `min`, `max`, `control`,
`category`).
3.4 `PlayLayer::update(float)` does not exist in 2.2081 → hook never fires
`class $modify(CBFPlayLayer, PlayLayer) { void update(float dt) { PlayLayer::update(dt); ... } }` has no such member in 2.2081. It is replaced by a `GJBaseGameLayer::update` hook gated
to `PlayLayer` instances (`typeinfo_cast<PlayLayer*>(this)`), because `LevelEditorLayer`
also derives from `GJBaseGameLayer` and must be left alone.
3.5 `pushButton` / `releaseButton` return `bool`, not `void`
Hook signatures corrected to `bool ...(PlayerButton)` with `return PlayerObject::...`.
3.6 C++20 does not compile against Geode 5
`loader/CMakeLists.txt:156` sets `cxx_std_23` PUBLIC, and
`Geode/platform/platform.hpp` contains a hard `static_assert(GEODE_CXX_STANDARD >= 202302L, "Geode requires C++23 support to build! ... change CMAKE_CXX_STANDARD from 20 to 23")`.
The Geode v5 changelog also states plainly that "mods targeting C++20 will no longer
compile". Fixed: `CMAKE_CXX_STANDARD 23`, and `target_compile_features(... cxx_std_20)`
removed.
3.7 `find_package(geode)` + `add_geode_mod()` is not the v5 build layout
The official v5 `example-mod` `CMakeLists.txt` does:
```cmake
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
```
with a plain `add_library(${PROJECT_NAME} SHARED ${SOURCES})`. The spec's
`add_geode_mod(...)`/`find_package(geode REQUIRED)` pair is not what a v5 SDK tree provides
out of the box. Rewritten to match the upstream template.
3.8 The original logic was a no-op
```cpp
void pushButton(PlayerButton p0) {
    if (g_modEnabled && g_savedTicks > 0) {
        log::debug("CBF [2.2081]: Intercepted pushButton with {} ticks saved.", g_savedTicks);
    }
    PlayerObject::pushButton(p0);
}
```
Logging and then forwarding the call unchanged cannot shift anything — `pushButton` is
already being executed on the tick the game chose. Worse, `PlayerObject::pushButton` is not
where input enters the simulation: the real funnel is
`GJBaseGameLayer::handleButton`, which also serves keyboard, mouse, controller and replay
input. The engine here hooks the funnel and performs an actual state rewind.
---
4. 2.2081 physics & input-sync notes
240 TPS lock. Since 2.2, physics runs at a fixed 240 steps/second regardless of
render FPS; a step is ~4.166 ms. FPS above 240 only reduces latency. This is why every
quantity in this mod is expressed in ticks (1–16), not milliseconds.
Click Between Frames → Click Between Steps. syzzi's CBF mod (April 2024) let inputs
register between visual frames; RobTop shipped an official equivalent, Click Between
Steps, in 2.208 (January 2026). CBF v1.5.0 was ported to 2.2081 by "switch[ing] to
RobTop's input handling", i.e. the vanilla timestamped queue — which is exactly the
`gd::vector<PlayerButtonCommand> m_queuedButtons` member in the 2.2081 bindings.
`PlayerButtonCommand` in 2.2081 carries `PlayerButton m_button`, `bool m_isPush`,
`bool m_isPlayer2`, `int m_step` and `double m_timestamp`. The timestamped queue is what
makes sub-step input possible in vanilla; it does not let an input be simulated
before the step it is queued for, which is the distinction between CBS (precision) and
this mod (time shift).
`geode::utils::getInputTimestamp()` is available in Geode 5 (added in
`v5.0.0-beta.2`) and is used here for verbose logging of the physical input time.
Timewarp / slow motion changes how many steps a rendered frame contains. The engine
does not depend on frame time (it works per step) and bails out while a level's resume
timer is active, but sub-tick behaviour under timewarp is untested.
---
5. What is not verified
Be explicit about this before publishing anything:
No compile. No Geode SDK / GD toolchain in this environment (see §1). API usage is
verified symbol-by-symbol, but the first `geode build` may still surface an
include-path or C++23 nits.
No runtime test. The rewind engine (`createCheckpoint` every step +
`loadFromCheckpoint` mid-run) has never executed against a live GD process here. It is
the highest-risk part of the design; `rollback-shift` exists so it can be turned off
while the safe measurement path is validated.
No leaderboard/anti-cheat statement. This modifies simulated state. Whether any
list, server or moderator tolerates it is a policy question, not a technical one.
