// Click Before Frames
// Target: Geometry Dash 2.2081 / Geode SDK 5.x (C++23)
//
// Every API used below was checked against the generated 2.2081 bindings
// (geode-sdk/bindings -> Codegen Win64) and the Geode v5.11.x loader headers.
// See RESEARCH.md for the exact declarations that were verified, and for the
// list of things in the original specification that do not exist in 2.2081.

#include <Geode/Geode.hpp>
#include <fmt/format.h>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace cbf {

// 2.2081 runs physics at a fixed 240 steps/second, so one step is ~4.166 ms.
// The maximum shift we allow is well below one 60 FPS frame (4 ticks) per
// press so that the correction can never be mistaken for a teleport.
constexpr int kMaxShiftTicks = 16;
constexpr int kRingCapacity   = kMaxShiftTicks + 2;
constexpr int kHudTag         = 998877;

// ---------------------------------------------------------------------------
// Settings mirror
// ---------------------------------------------------------------------------

struct Settings {
    bool enabled        = false;
    int  ticks          = 2;
    bool hud            = true;
    bool rollback       = true;
    bool practiceOk     = false;
    int  maxObjects     = 12000;
    bool debug          = false;
};

inline Settings g_cfg;

void loadSettings() {
    auto mod = Mod::get();
    if (!mod) return;

    g_cfg.enabled    = mod->getSettingValue<bool>("enabled");
    g_cfg.ticks      = static_cast<int>(mod->getSettingValue<int64_t>("tick-count"));
    g_cfg.hud        = mod->getSettingValue<bool>("show-hud");
    g_cfg.rollback   = mod->getSettingValue<bool>("rollback-shift");
    g_cfg.practiceOk = mod->getSettingValue<bool>("allow-in-practice");
    g_cfg.maxObjects = static_cast<int>(mod->getSettingValue<int64_t>("max-objects"));
    g_cfg.debug      = mod->getSettingValue<bool>("debug-log");

    g_cfg.ticks = std::clamp(g_cfg.ticks, 0, kMaxShiftTicks);
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

inline std::string g_lastHudText;
inline int g_shiftCount = 0;

CCLabelBMFont* ensureHud(PlayLayer* pl) {
    if (!pl) return nullptr;

    auto existing = typeinfo_cast<CCLabelBMFont*>(pl->getChildByTag(kHudTag));
    if (!g_cfg.hud) {
        if (existing) existing->removeFromParentAndCleanup(true);
        return nullptr;
    }
    if (existing) return existing;

    auto label = CCLabelBMFont::create("CBF", "chatFont.fnt");
    if (!label) return nullptr;

    label->setTag(kHudTag);
    label->setAnchorPoint({ 0.f, 1.f });
    label->setScale(0.5f);
    label->setZOrder(999);

    auto win = CCDirector::get()->getWinSize();
    label->setPosition({ 8.f, win.height - 8.f });

    pl->addChild(label);
    g_lastHudText.clear();
    return label;
}

std::string hudText() {
    if (!g_cfg.enabled) return "CBF: OFF";
    if (!g_cfg.rollback) return fmt::format("CBF: SAFE (measured {})", g_cfg.ticks);
    if (g_cfg.ticks <= 0) return "CBF: ON (0 ticks)";
    return fmt::format("CBF: ON [{} ticks] x{}", g_cfg.ticks, g_shiftCount);
}

void updateHud(PlayLayer* pl) {
    // Also (re)creates or removes the label when the setting is toggled mid-level.
    auto label = ensureHud(pl);
    if (!label) return;

    auto text = hudText();
    if (text != g_lastHudText) {
        label->setString(text.c_str());
        g_lastHudText = text;
    }
}

// ---------------------------------------------------------------------------
// Shift engine
// ---------------------------------------------------------------------------
//
// Terminology, because this is where the concept usually gets hand-waved:
//
//   * A "step" is one physics tick of GJBaseGameLayer::update (240/s).
//   * Geometry Dash consumes input *inside* GJBaseGameLayer::update, through
//     GJBaseGameLayer::handleButton. By the time we see a press there, the
//     tick it will be simulated on has already been decided.
//   * Therefore "clicking N ticks earlier" cannot be done by reordering the
//     input queue. The only honest implementation is to rewind the simulated
//     world by N ticks, re-assert the held input, and let the tick run again
//     from that earlier state. That is what this engine does.
//
// Timeline (shift = 2):
//
//   step S-2   step S-1   step S              step S+1
//     |          |         |                    |
//     |          |         press arrives ----+  |
//     |          |         (applied at S)     |  |
//     |          |                            v  |
//     +-------- restore snapshot, re-assert, run tick from here
//
// The cost is visible in wall-clock terms only: the level "loses" N ticks of
// progress per shifted press. At N=2 that is 8.3 ms, i.e. below one 60 FPS
// frame, and every shifted press is charged to the run.

class Engine {
public:
    static Engine& get() {
        static Engine instance;
        return instance;
    }

    void reset() {
        for (auto& ref : m_ring) ref = nullptr;
        m_count       = 0;
        m_pending     = 0;
        m_cooldown    = 0;
        m_lastShift   = 0;
        m_totalShifts = 0;
        m_perfWarned  = false;
        m_held.clear();
        m_seen = 0;
        g_shiftCount = 0;
    }

    // Called at the very top of GJBaseGameLayer::update, before vanilla runs.
    void onPreStep(GJBaseGameLayer* layer) {
        if (m_busy) return;
        BusyGuard guard(m_busy);

        auto playLayer = typeinfo_cast<PlayLayer*>(layer);
        if (!playLayer) return;

        if (!canOperate(playLayer)) {
            invalidate();
            return;
        }

        if (m_cooldown > 0) {
            --m_cooldown;
            invalidate();
            return;
        }

        if (m_pending > 0) {
            applyShift(playLayer);
            return;
        }

        capture(playLayer);
    }

    // Called from our GJBaseGameLayer::handleButton hook.
    void noteButton(bool down, int button, bool isPlayer1) {
        if (m_busy) return;
        if (!isPlayer1) return; // Player 2 shifting is deliberately unsupported.

        trackHeld(button, down);

        if (!down) return;
        if (!g_cfg.enabled || !g_cfg.rollback || g_cfg.ticks <= 0) return;

        if (m_count <= g_cfg.ticks) {
            if (g_cfg.debug) {
                log::debug("CBF: press ignored, ring only holds {} step(s)", m_count);
            }
            return;
        }

        m_pending = std::min(g_cfg.ticks, m_count - 1);
        if (g_cfg.debug) {
            log::debug("CBF: press queued for a {} tick shift", m_pending);
        }
    }

    int  lastShift() const   { return m_lastShift; }
    int  totalShifts() const { return m_totalShifts; }
    bool busy() const        { return m_busy; }
    int  depth() const       { return m_count; }

private:
    struct BusyGuard {
        bool& flag;
        explicit BusyGuard(bool& f) : flag(f) { flag = true; }
        ~BusyGuard() { flag = false; }
    };

    bool canOperate(PlayLayer* pl) const {
        if (!g_cfg.enabled) return false;
        if (!pl->m_player1) return false;
        if (pl->m_player1->m_isDead) return false;
        if (pl->m_isPaused) return false;
        if (pl->m_levelEndAnimationStarted) return false;
        if (pl->m_resumeTimer > 0) return false;
        if (pl->m_isPracticeMode && !g_cfg.practiceOk) return false;
        return true;
    }

    void invalidate() {
        for (auto& ref : m_ring) ref = nullptr;
        m_count   = 0;
        m_pending = 0;
    }

    void capture(PlayLayer* pl) {
        if (!g_cfg.rollback || g_cfg.ticks <= 0) {
            invalidate();
            return;
        }
        if (pl->m_objects && static_cast<int>(pl->m_objects->count()) > g_cfg.maxObjects) {
            if (!m_perfWarned) {
                m_perfWarned = true;
                log::warn(
                    "CBF: level has {} objects (> budget {}), snapshot rewind disabled",
                    pl->m_objects->count(), g_cfg.maxObjects
                );
            }
            invalidate();
            return;
        }

        // createCheckpoint() returns an autoreleased object; Ref adopts a retain
        // on it and releases it when the slot is overwritten.
        auto snapshot = pl->createCheckpoint();
        if (!snapshot) {
            invalidate();
            return;
        }

        for (int i = kRingCapacity - 1; i > 0; --i) {
            m_ring[i] = m_ring[i - 1];
        }
        m_ring[0] = snapshot;
        m_count   = std::min(m_count + 1, static_cast<int>(kRingCapacity));
    }

    void applyShift(PlayLayer* pl) {
        const int shift = m_pending;
        m_pending = 0;

        if (shift <= 0 || m_count <= shift || !m_ring[shift]) {
            invalidate();
            return;
        }

        // Hold our own reference so the restore target survives the ring purge.
        Ref<CheckpointObject> snapshot = m_ring[shift];

        // Everything newer than the restore point is now fiction.
        for (auto& ref : m_ring) ref = nullptr;
        m_count = 0;

        // Give the simulation a couple of clean ticks before the next capture.
        m_cooldown = 2;
        m_lastShift = shift;
        ++m_totalShifts;
        g_shiftCount = m_totalShifts;

        pl->loadFromCheckpoint(snapshot.data());
        reassertButtons(pl);

        if (g_cfg.debug) {
            log::debug("CBF: rewound {} ticks (total {})", shift, m_totalShifts);
        }
    }

    void trackHeld(int button, bool down) {
        if (button >= 1 && button <= 3) {
            m_seen |= (1 << button);
        }

        auto it = std::find(m_held.begin(), m_held.end(), button);
        if (down && it == m_held.end()) {
            m_held.push_back(button);
        }
        else if (!down && it != m_held.end()) {
            m_held.erase(it);
        }
    }

    // After restoring a checkpoint, the game state carries the button flags as
    // they were N ticks ago. Force them back to the player's real physical state
    // so the shifted press is actually held from the restored tick onwards.
    void reassertButtons(GJBaseGameLayer* layer) {
        for (int button = 1; button <= 3; ++button) {
            if (!(m_seen & (1 << button))) continue;

            const bool held = std::find(m_held.begin(), m_held.end(), button) != m_held.end();
            layer->handleButton(held, button, true);
        }
    }

    std::array<Ref<CheckpointObject>, kRingCapacity> m_ring;
    std::vector<int> m_held;
    int  m_count       = 0;
    int  m_pending     = 0;
    int  m_cooldown    = 0;
    int  m_lastShift   = 0;
    int  m_totalShifts = 0;
    int  m_seen        = 0;
    bool m_busy        = false;
    bool m_perfWarned  = false;
};

void onLevelInit(PlayLayer* pl) {
    loadSettings();
    Engine::get().reset();
    ensureHud(pl);
}

void onPostStep(PlayLayer* pl) {
    updateHud(pl);
}

// Counts *physical* presses (kept for parity with the spec's PlayerObject hook
// and for verbose logging). The functional interception is done on the
// GJBaseGameLayer::handleButton funnel, which sees keyboard, mouse, controller
// and replay input alike.
void onPlayerButton(PlayerObject* player, PlayerButton button, bool down) {
    if (!g_cfg.debug) return;
    if (!player || !PlayLayer::get()) return;

    log::debug(
        "CBF: PlayerObject::{}Button(PlayerButton::{}) at input time {:.4f}s",
        down ? "push" : "release",
        static_cast<int>(button),
        geode::utils::getInputTimestamp()
    );
}

} // namespace cbf

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// 2.2081 note: PlayLayer does NOT declare update(float) -- the step function is
// GJBaseGameLayer::update(float) (win 0x237850). Hooking PlayLayer::update is
// not a thing in this version; the inherited base method is hooked instead and
// gated to PlayLayer instances so the editor is unaffected.
class $modify(CBFGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto playLayer = typeinfo_cast<PlayLayer*>(this);

        if (playLayer) cbf::Engine::get().onPreStep(this);

        GJBaseGameLayer::update(dt);

        if (playLayer) cbf::onPostStep(playLayer);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        cbf::Engine::get().noteButton(down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(CBFPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        cbf::onLevelInit(this);
        return true;
    }

    void resetLevel() {
        cbf::Engine::get().reset();
        PlayLayer::resetLevel();
    }
};

class $modify(CBFPlayerObject, PlayerObject) {
    // Both of these return bool in 2.2081 (win 0x397f40 / win 0x3981d0).
    bool pushButton(PlayerButton button) {
        cbf::onPlayerButton(this, button, true);
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        cbf::onPlayerButton(this, button, false);
        return PlayerObject::releaseButton(button);
    }
};

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    cbf::loadSettings();

    log::info(
        "Click Before Frames loaded for GD 2.2081 (enabled={}, ticks={}, rewind={})",
        cbf::g_cfg.enabled, cbf::g_cfg.ticks, cbf::g_cfg.rollback
    );

    listenForSettingChanges<bool>("enabled", [](bool value) {
        cbf::g_cfg.enabled = value;
        if (!value) cbf::Engine::get().reset();
        log::info("CBF: enabled = {}", value);
    });

    listenForSettingChanges<int64_t>("tick-count", [](int64_t value) {
        cbf::g_cfg.ticks = std::clamp(static_cast<int>(value), 0, cbf::kMaxShiftTicks);
        log::info("CBF: ticks = {}", cbf::g_cfg.ticks);
    });

    listenForSettingChanges<bool>("show-hud", [](bool value) {
        cbf::g_cfg.hud = value;
    });

    listenForSettingChanges<bool>("rollback-shift", [](bool value) {
        cbf::g_cfg.rollback = value;
        cbf::Engine::get().reset();
        log::info("CBF: snapshot rewind = {}", value);
    });

    listenForSettingChanges<bool>("allow-in-practice", [](bool value) {
        cbf::g_cfg.practiceOk = value;
    });

    listenForSettingChanges<int64_t>("max-objects", [](int64_t value) {
        cbf::g_cfg.maxObjects = static_cast<int>(value);
    });

    listenForSettingChanges<bool>("debug-log", [](bool value) {
        cbf::g_cfg.debug = value;
    });

    // v5 keybind API: the listener receives (Keybind const&, down, repeat, timestamp).
    listenForKeybindSettingPresses(
        "toggle-key",
        [](Keybind const& keybind, bool down, bool repeat, double) -> bool {
            if (!down || repeat) return false;

            auto mod = Mod::get();

            // Shift + the bound key cycles the saved tick count, bare key toggles.
            bool shiftHeld = keybind.modifiers & KeyboardModifier::Shift;

            if (shiftHeld) {
                auto next = (cbf::g_cfg.ticks + 1) % (cbf::kMaxShiftTicks + 1);
                mod->setSettingValue<int64_t>("tick-count", next);
                log::info("CBF: cycled to {} ticks", next);
            }
            else {
                bool nowEnabled = !cbf::g_cfg.enabled;
                mod->setSettingValue<bool>("enabled", nowEnabled);
                if (nowEnabled && cbf::g_cfg.ticks == 0) {
                    mod->setSettingValue<int64_t>("tick-count", 2);
                }
                log::info("CBF: toggled {}", nowEnabled ? "on" : "off");
            }
            return false;
        }
    );
}
