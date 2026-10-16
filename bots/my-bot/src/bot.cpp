// Deterministic my-bot v3:
// - Absolute tribute priority whenever carrying gems
// - Full draft-memory precomputation (camps, altars, lane assignments)
// - 8-byte shared-memory live team coordination
// - Coordinated enemy chasing with dashes and situational TP crystals
// - Defensive memory-safe model (no dangling pointers)
#include <cambc.h>
#include "bot.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <variant>
#include <vector>

using cambc::team;
using cambc::vec2;

namespace {

// =========================================================================
//  Constants
// =========================================================================
constexpr int CHASE_MAX_DIST     = 12;  // max distance to chase enemy via shared memory
constexpr int ENEMY_STALE_ROUNDS = 6;   // shared-mem sighting expires after 6 rounds

// =========================================================================
//  Shared-memory helpers  (8 bytes, see bot.h for layout)
// =========================================================================
struct SMem {
    uint8_t data[8]{};

    // --- tribute mask (byte 0) ---
    bool is_tributing(int lane) const { return (data[0] >> lane) & 1; }
    void set_tributing(int lane, bool v) {
        if (v) data[0] |= static_cast<uint8_t>(1u << lane);
        else   data[0] &= static_cast<uint8_t>(~(1u << lane));
    }
    int tribute_count() const {
        int c = 0;
        for (int i = 0; i < 5; ++i) c += is_tributing(i);
        return c;
    }

    // --- enemy sighting (bytes 1-4) ---
    bool has_enemy() const { return data[1] != 0xFF; }
    vec2 enemy_pos() const { return {data[1], data[2]}; }
    int  enemy_gems() const { return data[3]; }
    uint8_t report_round() const { return data[4]; }
    void set_enemy(vec2 pos, int gems, uint8_t rnd) {
        data[1] = static_cast<uint8_t>(pos.x);
        data[2] = static_cast<uint8_t>(pos.y);
        data[3] = static_cast<uint8_t>(gems);
        data[4] = rnd;
    }
    void clear_enemy() { data[1] = 0xFF; data[2] = 0xFF; data[3] = 0; }
    bool enemy_is_stale(uint8_t cur_round) const {
        int age = (cur_round >= data[4])
                    ? (cur_round - data[4])
                    : (256 - data[4] + cur_round);
        return age > ENEMY_STALE_ROUNDS;
    }

    // --- altar locks (byte 5) ---
    bool altar_locked(int idx) const { return (data[5] >> idx) & 1; }
    void set_altar_lock(int idx, bool v) {
        if (v) data[5] |= static_cast<uint8_t>(1u << idx);
        else   data[5] &= static_cast<uint8_t>(~(1u << idx));
    }

    // --- chaser lane (byte 6, 0xFF = none) ---
    int  chaser_lane() const { return data[6] == 0xFF ? -1 : data[6]; }
    void set_chaser(int lane) { data[6] = static_cast<uint8_t>(lane); }
    void clear_chaser_if(int lane) { if (data[6] == static_cast<uint8_t>(lane)) data[6] = 0xFF; }

    static SMem read() {
        SMem s;
        cambc::shared_memory.read(0, std::span<uint8_t>{s.data, 8});
        return s;
    }
    void write() const {
        cambc::shared_memory.write(0, std::span<const uint8_t>{data, 8});
    }
};

// =========================================================================
//  Geometry / pathfinding
// =========================================================================
int cheb(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

bool move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;
    if (goals.count() == 1 && !blocked.is_set(goals.positions()[0]))
        return me.move_towards(goals.positions()[0]);

    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step))
            step = tile;
    return step != me.position() && me.move_absolute(step);
}

bool move_towards_pos(vec2 target) {
    cambc::bitboard goal;
    goal.clear();
    goal.set(target, true);
    goal.draw_chebyshev(target, 1);
    return move_towards_closest(goal);
}

// =========================================================================
//  Combat helpers
// =========================================================================
template <class Target>
bool attack_if_in_range(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0
        || !me.attack_tiles().is_set(target.position()))
        return false;
    me.attack(target);
    return true;
}

bool ability_excludes_move(const cambc::ability& abil) {
    try {
        const auto& opt = abil.info();
        if (!opt) return false;
        return std::visit([](const auto& v) { return v.exclusive_with_movement; }, *opt);
    } catch (...) { return false; }
}

// =========================================================================
//  Roster / build helpers
// =========================================================================
int my_lane() {
    const auto allies = cambc::champions(team::ally);
    const auto& me = cambc::self();
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (allies[i] == me) return static_cast<int>(i);
    return 0;
}

cambc::item_type build_for(cambc::champion_type t) {
    using ct = cambc::champion_type;
    using it = cambc::item_type;
    switch (t) {
        case ct::vesper_vale: return it::treasure_shard_of_strength;
        case ct::vendetta:
        case ct::ren:
        case ct::varg:
        case ct::issac:     return it::monster_shard_of_strength;
        case ct::ryth:
        case ct::marcella:  return it::jungle_shard_of_health;
        case ct::willow:
        case ct::stellar:
        case ct::cassi:
        case ct::prism:
        case ct::tavi:      return it::power_shard_of_focus;
        default:            return it::power_shard_of_strength;
    }
}

int priority_for(cambc::champion_type t, bool carrying) {
    if (carrying) return 100;
    using ct = cambc::champion_type;
    switch (t) {
        case ct::willow:      return 90;
        case ct::vesper_vale: return 70;
        case ct::vendetta:    return 65;
        case ct::ren:         return 60;
        case ct::varg:        return 58;
        case ct::ryth:        return 55;
        case ct::marcella:    return 54;
        case ct::stellar:     return 45;
        case ct::prism:       return 44;
        case ct::tavi:        return 43;
        case ct::cassi:       return 42;
        case ct::issac:       return 41;
        default:              return 50;
    }
}

// =========================================================================
//  Abilities: nuke (exclusive-movement) + support (compatible)
// =========================================================================
bool try_nuke() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& abil : me.abilities()) {
        try {
            if (!abil.is_ready() || !ability_excludes_move(abil))
                continue;
            if (!abil.can_target_enemies() && !abil.can_target_camps())
                continue;
            cambc::bitboard range;
            try { range = abil.valid_tiles(); } catch (...) { continue; }

            std::optional<cambc::champion> best_e;
            for (const auto& e : cambc::vision::champions(team::enemy)) {
                if (!e.is_visible() || e.health() <= 0 || !range.is_set(e.position()))
                    continue;
                if (!best_e || e.health() < best_e->health())
                    best_e = e;
            }
            if (best_e && abil.can_target_enemies()) {
                if (abil.use(*best_e)) return true;
                return false;
            }
            std::optional<cambc::camp> best_c;
            for (const auto& c : cambc::vision::camps()) {
                if (!c.is_visible() || c.health() <= 0 || !range.is_set(c.position()))
                    continue;
                if (c.max_health() > 5000.0f) continue;
                if (!best_c || c.health() < best_c->health())
                    best_c = c;
            }
            if (best_c && abil.can_target_camps()) {
                if (abil.use(*best_c)) return true;
                return false;
            }
        } catch (...) {}
    }
    return false;
}

void try_support_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast()) return;
    for (auto& abil : me.abilities()) {
        if (!me.can_cast()) return;
        try {
            if (!abil.is_ready() || ability_excludes_move(abil))
                continue;

            // Attack reset: double-hit when target is in basic range.
            if (abil.is_attack_reset() && abil.can_target_self()) {
                bool near = false;
                for (const auto& e : cambc::vision::champions(team::enemy))
                    if (e.is_visible() && me.attack_tiles().is_set(e.position())) { near = true; break; }
                if (!near)
                    for (const auto& c : cambc::vision::camps())
                        if (c.is_visible() && me.attack_tiles().is_set(c.position())) { near = true; break; }
                if (near && abil.use()) return;
                continue;
            }
            // Shield when below 75% and enemies/camps nearby.
            if (abil.is_shield() && abil.can_target_self()) {
                if (me.health() >= me.max_health() * 0.75f) continue;
                bool threat = !cambc::vision::champions(team::enemy).empty()
                           || !cambc::vision::camps().empty();
                if (threat && abil.use()) return;
                continue;
            }
            // Heal most-damaged ally.
            if (abil.is_heal() && abil.can_target_allies()) {
                cambc::bitboard range;
                try { range = abil.valid_tiles(); } catch (...) { continue; }
                std::optional<cambc::champion> worst;
                for (const auto& a : cambc::vision::champions(team::ally)) {
                    if (a == me || !a.is_visible() || a.health() <= 0)
                        continue;
                    if (a.health() >= a.max_health() * 0.85f) continue;
                    if (!range.is_set(a.position())) continue;
                    if (!worst || (a.max_health() - a.health()) > (worst->max_health() - worst->health()))
                        worst = a;
                }
                if (worst && abil.use(*worst)) return;
                continue;
            }
            // Damage/CC on lowest-health enemy.
            if ((abil.is_damage() || abil.is_cc()) && !ability_excludes_move(abil)) {
                cambc::bitboard range;
                try { range = abil.valid_tiles(); } catch (...) { continue; }
                std::optional<cambc::champion> best;
                for (const auto& e : cambc::vision::champions(team::enemy)) {
                    if (!e.is_visible() || e.health() <= 0 || !range.is_set(e.position()))
                        continue;
                    if (!best || e.health() < best->health())
                        best = e;
                }
                if (best && abil.can_target_enemies()) {
                    if (abil.use(*best)) return;
                }
            }
        } catch (...) {}
    }
}

// =========================================================================
//  Dash toward a target position
// =========================================================================
bool try_dash_toward(vec2 target) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& abil : me.abilities()) {
        try {
            if (!abil.is_ready() || !abil.is_dash()) continue;
            if (!abil.can_target_ground()) continue;

            cambc::bitboard valid;
            try { valid = abil.valid_tiles(); } catch (...) { continue; }

            vec2 best = me.position();
            int best_d = cheb(me.position(), target);
            for (auto tile : valid.positions()) {
                int d = cheb(tile, target);
                if (d < best_d) { best_d = d; best = tile; }
            }
            if (best != me.position() && abil.use(best))
                return true;
        } catch (...) {}
    }
    return false;
}

// =========================================================================
//  TP crystal toward target (primarily for jumping walls/obstacles)
// =========================================================================
bool try_tp_toward(vec2 target) {
    auto& me = cambc::self();
    try {
        auto& inv = me.consumables();
        if (inv[cambc::consumable_type::tp_crystal].count() == 0)
            return false;

        const auto walls = cambc::map::walls() | cambc::map::holes()
            | cambc::map::altars() | cambc::map::spawn_region(team::enemy)
            | cambc::vision::champion_tiles() | cambc::vision::camp_tiles();

        vec2 best = me.position();
        int best_d = cheb(me.position(), target);
        for (int dx = -3; dx <= 3; ++dx) {
            for (int dy = -3; dy <= 3; ++dy) {
                if (std::max(std::abs(dx), std::abs(dy)) > 3) continue;
                vec2 dest{me.position().x + dx, me.position().y + dy};
                if (!cambc::map::is_in_bounds(dest)) continue;
                if (walls.is_set(dest)) continue;
                int d = cheb(dest, target);
                if (d < best_d) { best_d = d; best = dest; }
            }
        }

        if (best != me.position()) {
            return inv[cambc::consumable_type::tp_crystal].use(best);
        }
    } catch (...) {}
    return false;
}

// =========================================================================
//  Ward placement
// =========================================================================
void maybe_ward() {
    auto& me = cambc::self();
    try {
        if (me.ward_cooldown() != 0) return;
        if (!me.ward_positions().empty()) return;
        const auto placement = me.ward_placement_tiles();
        if (placement.is_empty()) return;
        vec2 anchor = me.position();
        int best = std::numeric_limits<int>::max();
        for (auto altar : cambc::map::altars().positions()) {
            int d = cheb(me.position(), altar);
            if (d < best) { best = d; anchor = altar; }
        }
        if (best == std::numeric_limits<int>::max()) {
            for (const auto& c : cambc::vision::camps()) {
                int d = cheb(me.position(), c.position());
                if (d < best) { best = d; anchor = c.position(); }
            }
        }
        for (auto tile : placement.positions()) {
            if (cheb(tile, anchor) < cheb(me.position(), anchor)) {
                me.place_ward(tile);
                return;
            }
        }
    } catch (...) {}
}

// =========================================================================
//  Scan visible enemies / altars and update shared memory
// =========================================================================
void scan_enemies_and_altars(SMem& sm) {
    const uint8_t cur_round = static_cast<uint8_t>(cambc::round_number() & 0xFF);

    // Clear stale sightings.
    if (sm.has_enemy() && sm.enemy_is_stale(cur_round))
        sm.clear_enemy();

    // Find highest-gem visible enemy champion.
    std::optional<cambc::champion> best;
    for (const auto& e : cambc::vision::champions(team::enemy)) {
        try {
            if (!e.is_visible() || e.health() <= 0) continue;
            if (!best || e.gem_count() > best->gem_count()
                || (e.gem_count() == best->gem_count() && e.health() < best->health()))
                best = e;
        } catch (...) {}
    }
    if (best && best->gem_count() > 0) {
        if (!sm.has_enemy() || static_cast<int>(best->gem_count()) > sm.enemy_gems()) {
            sm.set_enemy(best->position(), static_cast<int>(best->gem_count()), cur_round);
        }
    }

    // Update altar locks.
    int ai = 0;
    for (auto altar : cambc::map::altars().positions()) {
        try {
            sm.set_altar_lock(ai, cambc::vision::has_altar_lock(altar));
        } catch (...) {}
        ++ai;
    }
}

// =========================================================================
//  TRIBUTE: Top Priority Action when holding ANY gems
// =========================================================================
bool try_tribute(SMem& sm, int lane) {
    auto& me = cambc::self();
    if (me.gem_count() == 0) return false;

    // 1. Are we currently in submit_gems_range of an altar?
    for (auto altar : cambc::map::altars().positions()) {
        if (cheb(me.position(), altar) <= cambc::constants::submit_gems_range) {
            bool has_lock = false;
            try { has_lock = cambc::vision::has_altar_lock(altar); } catch (...) {}

            if (has_lock) {
                // Enemy lock on this altar: attack the lock to break it!
                for (const auto& lock : cambc::vision::altar_locks()) {
                    try {
                        if (lock.is_enemy() && me.attack_tiles().is_set(lock.position())) {
                            me.attack(lock);
                            me.set_priority(100);
                            sm.set_tributing(lane, true);
                            return true;
                        }
                    } catch (...) {}
                }
            } else {
                // Altar is clear: place our lock if we have one.
                try {
                    auto& inv = me.consumables();
                    if (inv[cambc::consumable_type::altar_lock].count() > 0)
                        inv[cambc::consumable_type::altar_lock].use(altar);
                } catch (...) {}

                // Submit gems!
                try {
                    me.submit_gems(altar);
                    me.set_priority(100);
                    sm.set_tributing(lane, true);
                    return true;
                } catch (...) {}
            }
        }
    }

    // 2. Not in range yet: navigate to nearest altar approach tiles.
    cambc::bitboard goals;
    goals.clear();
    cambc::bitboard unlocked_goals;
    unlocked_goals.clear();

    for (auto altar : cambc::map::altars().positions()) {
        auto approach = tiles_around(altar, cambc::constants::submit_gems_range);
        goals |= approach;
        bool locked = false;
        try { locked = cambc::vision::has_altar_lock(altar); } catch (...) {}
        if (!locked) unlocked_goals |= approach;
    }

    cambc::bitboard target_goals = unlocked_goals.is_empty() ? goals : unlocked_goals;

    // Dash toward closest altar if dash ability is ready
    vec2 nearest_altar{};
    int min_d = 999;
    for (auto a : cambc::map::altars().positions()) {
        int d = cheb(me.position(), a);
        if (d < min_d) { min_d = d; nearest_altar = a; }
    }
    if (min_d < 999) {
        try_dash_toward(nearest_altar);
    }

    // Move toward altar approach tiles
    move_towards_closest(target_goals);

    // After moving, if we reached submit range, submit immediately!
    for (auto altar : cambc::map::altars().positions()) {
        if (cheb(me.position(), altar) <= cambc::constants::submit_gems_range) {
            bool has_lock = false;
            try { has_lock = cambc::vision::has_altar_lock(altar); } catch (...) {}
            if (!has_lock) {
                try {
                    auto& inv = me.consumables();
                    if (inv[cambc::consumable_type::altar_lock].count() > 0)
                        inv[cambc::consumable_type::altar_lock].use(altar);
                } catch (...) {}
                try {
                    me.submit_gems(altar);
                    break;
                } catch (...) {}
            }
        }
    }

    // Melee attack enemy lock or enemy champion if in range after moving
    if (me.can_attack()) {
        for (const auto& lock : cambc::vision::altar_locks()) {
            try {
                if (lock.is_enemy() && me.attack_tiles().is_set(lock.position())) {
                    me.attack(lock);
                    break;
                }
            } catch (...) {}
        }
        if (me.can_attack()) {
            for (const auto& enemy : cambc::vision::champions(team::enemy)) {
                if (attack_if_in_range(enemy)) break;
            }
        }
    }

    try_support_abilities();
    me.set_priority(100);
    sm.set_tributing(lane, true);
    return true;
}

// =========================================================================
//  CHASE: Hunt enemy gem carriers when carrying few/no gems
// =========================================================================
bool try_chase_enemy(SMem& sm, int lane) {
    auto& me = cambc::self();

    // Don't chase if low HP or carrying many gems (tribute first!)
    if (me.health() < me.max_health() * 0.45f) return false;
    if (me.gem_count() > 0) return false;

    // Check visible enemies carrying gems
    std::optional<cambc::champion> target;
    for (const auto& e : cambc::vision::champions(team::enemy)) {
        try {
            if (!e.is_visible() || e.health() <= 0 || e.gem_count() == 0) continue;
            if (!target || e.gem_count() > target->gem_count()
                || (e.gem_count() == target->gem_count() && e.health() < target->health()))
                target = e;
        } catch (...) {}
    }

    vec2 chase_pos{};
    if (target) {
        chase_pos = target->position();
        sm.set_chaser(lane);
    } else if (sm.has_enemy() && sm.enemy_gems() > 0) {
        const uint8_t cur = static_cast<uint8_t>(cambc::round_number() & 0xFF);
        if (!sm.enemy_is_stale(cur)) {
            int d = cheb(me.position(), sm.enemy_pos());
            if (d <= CHASE_MAX_DIST && (sm.chaser_lane() == -1 || sm.chaser_lane() == lane)) {
                chase_pos = sm.enemy_pos();
                sm.set_chaser(lane);
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }

    // Try pre-movement nuke if target in range
    if (target) {
        try {
            for (auto& abil : me.abilities()) {
                if (!abil.is_ready() || !ability_excludes_move(abil)) continue;
                if (!abil.can_target_enemies()) continue;
                cambc::bitboard range;
                try { range = abil.valid_tiles(); } catch (...) { continue; }
                if (range.is_set(target->position())) {
                    if (abil.use(*target)) break;
                }
            }
        } catch (...) {}
    }

    // Attack if in range
    if (target && attack_if_in_range(*target)) {
        try_support_abilities();
        me.set_priority(priority_for(me.champion_name(), false));
        return true;
    }

    // Dash towards enemy
    try_dash_toward(chase_pos);

    // If target is across a wall/obstacle within 3 tiles, use TP crystal to jump wall
    if (!me.can_attack() && cheb(me.position(), chase_pos) <= 3) {
        try_tp_toward(chase_pos);
        if (me.is_teleporting()) {
            me.set_priority(80);
            return true;
        }
    }

    // Move toward enemy
    move_towards_pos(chase_pos);

    // Attack after moving
    if (target && attack_if_in_range(*target))
        try_support_abilities();

    me.set_priority(priority_for(me.champion_name(), false));
    return true;
}

// =========================================================================
//  Farming
// =========================================================================
void farm_camps(int lane, int assigned_camp) {
    (void)lane;
    (void)assigned_camp;
    for (const auto& camp : cambc::vision::camps()) {
        if (camp.max_health() > 5000.0f) continue;
        if (attack_if_in_range(camp)) return;
    }

    cambc::bitboard goals;
    goals.clear();
    const auto visible = cambc::vision::visible_tiles();
    const auto groups = cambc::map::camp_groups();
    for (const auto& group : groups) {
        bool group_visible = !(group.spawn_tiles() & visible).is_empty();
        if (group_visible
            && std::holds_alternative<cambc::map::camp_group_respawning>(group.state())) {
            continue;
        }
        // Don't solo monolith (drops 200 gems, has 20,000 HP)
        if (group.gem_value() >= 150) {
            continue;
        }
        goals |= group.spawn_tiles();
    }

    if (move_towards_closest(goals)) {
        for (const auto& camp : cambc::vision::camps()) {
            if (camp.max_health() > 5000.0f) continue;
            if (attack_if_in_range(camp)) return;
        }
    }
}

}  // namespace

// =========================================================================
//  Bot initialization
// =========================================================================
void Bot::init() {
    auto& me = cambc::self();
    build_item = build_for(me.champion_name());
    lane = my_lane();

    // Parse precomputed draft memory
    auto dm = cambc::draft_memory;
    if (dm.size() >= 16
        && static_cast<char>(dm[0]) == 'M'
        && static_cast<char>(dm[1]) == 'B'
        && static_cast<char>(dm[2]) == '0'
        && static_cast<char>(dm[3]) == '3') {
        num_groups = static_cast<int>(static_cast<uint8_t>(dm[4]));
        if (lane >= 0 && lane < 5) {
            assigned_camp = static_cast<int>(static_cast<uint8_t>(dm[6 + lane]));
        }
        for (int gi = 0; gi < num_groups && static_cast<std::size_t>(16 + gi * 2 + 1) < dm.size(); ++gi) {
            group_altar[gi]   = static_cast<int>(static_cast<uint8_t>(dm[16 + gi * 2]));
            group_altar_d[gi] = static_cast<int>(static_cast<uint8_t>(dm[16 + gi * 2 + 1]));
        }
        draft_parsed = true;
    }

    initialized = true;
}

// =========================================================================
//  Shopping
// =========================================================================
void Bot::buy_items(bool at_spawn) {
    auto& me = cambc::self();
    const bool is_tavi = me.champion_name() == cambc::champion_type::tavi;
    if (at_spawn || !me.is_alive() || is_tavi) {
        try {
            const unsigned count = me.gold_count() / build_item.cost();
            if (count && me.can_buy(build_item, count))
                me.buy(build_item, count);
        } catch (...) {}
    }
    // Consumables: one lock + one crystal.
    if (at_spawn || !me.is_alive()) {
        try {
            auto& inv = me.consumables();
            if (inv[cambc::consumable_type::altar_lock].count() == 0
                && me.can_buy(cambc::consumable_type::altar_lock))
                me.buy(cambc::consumable_type::altar_lock);
        } catch (...) {}
        try {
            auto& inv = me.consumables();
            if (inv[cambc::consumable_type::tp_crystal].count() == 0
                && me.can_buy(cambc::consumable_type::tp_crystal))
                me.buy(cambc::consumable_type::tp_crystal);
        } catch (...) {}
    }
}

// =========================================================================
//  MAIN TURN
// =========================================================================
void Bot::play_turn() {
    if (cambc::fuel_remaining() < 2000) {
        cambc::end_turn();
        return;
    }
    if (!initialized) init();

    auto& me = cambc::self();
    lane = my_lane();

    // Read shared memory
    SMem sm = SMem::read();

    // Dead: shop and clear status
    if (!me.is_alive()) {
        buy_items(false);
        sm.set_tributing(lane, false);
        sm.clear_chaser_if(lane);
        sm.write();
        return;
    }

    // Active channels: let them finish
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;

    // Cannot act this turn
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) {
        buy_items(false);
        return;
    }

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    buy_items(at_spawn);

    // Update shared memory from our current vision
    scan_enemies_and_altars(sm);

    // -----------------------------------------------------------------
    //  1. SURVIVAL
    // -----------------------------------------------------------------
    if (!at_spawn && me.health() < me.max_health() * 0.35f) {
        bool near_altar = false;
        for (auto a : cambc::map::altars().positions()) {
            if (cheb(me.position(), a) <= cambc::constants::submit_gems_range) {
                near_altar = true;
                break;
            }
        }
        if (!near_altar && (me.health() < me.max_health() * 0.25f || me.gold_count() >= 30)) {
            try { me.recall(); } catch (...) {}
            sm.set_tributing(lane, false);
            sm.clear_chaser_if(lane);
            sm.write();
            return;
        }
    }

    // -----------------------------------------------------------------
    //  2. TRIBUTE PRIORITY (top priority when holding ANY gems)
    // -----------------------------------------------------------------
    if (me.gem_count() > 0) {
        if (try_tribute(sm, lane)) {
            sm.write();
            return;
        }
    }

    // -----------------------------------------------------------------
    //  3. CHASE ENEMY GEM CARRIERS (when we have 0 gems)
    // -----------------------------------------------------------------
    if (try_chase_enemy(sm, lane)) {
        sm.write();
        return;
    }

    // -----------------------------------------------------------------
    //  4. ATTACK ENEMIES IN RANGE
    // -----------------------------------------------------------------
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (attack_if_in_range(enemy)) {
            try_support_abilities();
            me.set_priority(priority_for(me.champion_name(), false));
            sm.clear_chaser_if(lane);
            sm.write();
            return;
        }
    }

    // -----------------------------------------------------------------
    //  5. FARM CAMPS
    // -----------------------------------------------------------------
    sm.set_tributing(lane, false);
    sm.clear_chaser_if(lane);

    bool cast_exclusive = try_nuke();

    for (const auto& camp : cambc::vision::camps()) {
        if (camp.max_health() > 5000.0f) continue;
        if (attack_if_in_range(camp)) {
            try_support_abilities();
            maybe_ward();
            me.set_priority(priority_for(me.champion_name(), false));
            sm.write();
            return;
        }
    }

    if (!cast_exclusive)
        farm_camps(lane, assigned_camp);

    try_support_abilities();
    maybe_ward();
    me.set_priority(priority_for(me.champion_name(), false));
    sm.write();

    if (cambc::fuel_remaining() < 1000)
        cambc::end_turn();
}
