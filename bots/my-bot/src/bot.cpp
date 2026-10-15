// Deterministic my-bot: farm, tribute, shop, abilities, wards, locks.
// No randomness anywhere: lanes and builds derive from roster order + type.
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

// Same pathfinder as starter: deterministic distance field toward goals.
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
        if (!opt)
            return false;
        return std::visit([](const auto& v) { return v.exclusive_with_movement; }, *opt);
    } catch (...) {
        return false;
    }
}

// Index of self within ally roster order; deterministic lane 0..n-1.
int my_lane() {
    const auto allies = cambc::champions(team::ally);
    const auto& me = cambc::self();
    for (std::size_t i = 0; i < allies.size(); ++i) {
        if (allies[i] == me)
            return static_cast<int>(i);
    }
    return 0;
}

cambc::item_type build_for(cambc::champion_type t) {
    using ct = cambc::champion_type;
    using it = cambc::item_type;
    switch (t) {
        case ct::vesper_vale: return it::treasure_shard_of_strength;  // stacks with +20% passive
        case ct::vendetta:
        case ct::ren:
        case ct::varg:
        case ct::issac: return it::monster_shard_of_strength;  // +10% camp damage
        case ct::ryth:
        case ct::marcella: return it::jungle_shard_of_health;  // survive camps/frontline
        case ct::willow:
        case ct::stellar:
        case ct::cassi:
        case ct::prism:
        case ct::tavi: return it::power_shard_of_focus;  // spells + heal scale focus
        default: return it::power_shard_of_strength;
    }
}

int priority_for(cambc::champion_type t, bool carrying) {
    if (carrying)
        return 100;  // tributers first
    using ct = cambc::champion_type;
    switch (t) {
        case ct::willow: return 90;  // healer acts early
        case ct::vesper_vale: return 70;
        case ct::vendetta: return 65;
        case ct::ren: return 60;
        case ct::varg: return 58;
        case ct::ryth: return 55;
        case ct::marcella: return 54;
        case ct::stellar: return 45;
        case ct::prism: return 44;
        case ct::tavi: return 43;
        case ct::cassi: return 42;
        case ct::issac: return 41;
        default: return 50;
    }
}

// Try one excluding-movement nuke BEFORE moving. Returns true if cast.
bool try_nuke() {
    auto& me = cambc::self();
    if (!me.can_cast())
        return false;
    for (auto& abil : me.abilities()) {
        try {
            if (!abil.is_ready())
                continue;
            if (!ability_excludes_move(abil))
                continue;
            if (!abil.can_target_enemies() && !abil.can_target_camps())
                continue;
            cambc::bitboard range;
            try {
                range = abil.valid_tiles();
            } catch (...) {
                continue;
            }
            // Prefer lowest-health visible enemy champion in range.
            const cambc::champion* best_enemy = nullptr;
            for (const auto& e : cambc::vision::champions(team::enemy)) {
                if (!e.is_visible() || e.health() <= 0)
                    continue;
                if (!range.is_set(e.position()))
                    continue;
                if (!best_enemy || e.health() < best_enemy->health())
                    best_enemy = &e;
            }
            if (best_enemy && abil.can_target_enemies()) {
                if (abil.use(*best_enemy))
                    return true;
                // Cast failed (now-visible blocker reveals tile); caller moves on.
                return false;
            }
            // Otherwise hit lowest-health camp in range.
            const cambc::camp* best_camp = nullptr;
            for (const auto& c : cambc::vision::camps()) {
                if (!c.is_visible() || c.health() <= 0)
                    continue;
                if (!range.is_set(c.position()))
                    continue;
                if (!best_camp || c.health() < best_camp->health())
                    best_camp = &c;
            }
            if (best_camp && abil.can_target_camps()) {
                if (abil.use(*best_camp))
                    return true;
                return false;
            }
        } catch (...) {
            // Invalid target/visibility: try next slot.
        }
    }
    return false;
}

// Compatible abilities AFTER moving: resets, shields, heals, stuns, dashes.
void try_support_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast())
        return;
    for (auto& abil : me.abilities()) {
        if (!me.can_cast())
            return;  // one successful cast per turn
        try {
            if (!abil.is_ready())
                continue;
            if (ability_excludes_move(abil))
                continue;
            if (abil.is_attack_reset() && abil.can_target_self()) {
                // Double-hit when something is already in basic range.
                bool target_near = false;
                for (const auto& e : cambc::vision::champions(team::enemy)) {
                    if (e.is_visible() && me.attack_tiles().is_set(e.position())) {
                        target_near = true;
                        break;
                    }
                }
                if (!target_near) {
                    for (const auto& c : cambc::vision::camps()) {
                        if (c.is_visible() && me.attack_tiles().is_set(c.position())) {
                            target_near = true;
                            break;
                        }
                    }
                }
                if (target_near && abil.use())
                    return;
                continue;
            }
            if (abil.is_shield() && abil.can_target_self()) {
                if (me.health() >= me.max_health() * 0.7f)
                    continue;
                bool threat = !cambc::vision::champions(team::enemy).empty()
                    || !cambc::vision::camps().empty();
                if (threat && abil.use())
                    return;
                continue;
            }
            if (abil.is_heal() && abil.can_target_allies()) {
                cambc::bitboard range;
                try {
                    range = abil.valid_tiles();
                } catch (...) {
                    continue;
                }
                const cambc::champion* worst = nullptr;
                for (const auto& a : cambc::vision::champions(team::ally)) {
                    if (a == me || !a.is_visible() || a.health() <= 0)
                        continue;
                    if (a.health() >= a.max_health() * 0.8f)
                        continue;
                    if (!range.is_set(a.position()))
                        continue;
                    if (!worst || (a.max_health() - a.health()) > (worst->max_health() - worst->health()))
                        worst = &a;
                }
                if (worst && abil.use(*worst))
                    return;
                continue;
            }
            if ((abil.is_damage() || abil.is_cc()) && !ability_excludes_move(abil)) {
                cambc::bitboard range;
                try {
                    range = abil.valid_tiles();
                } catch (...) {
                    continue;
                }
                const cambc::champion* best = nullptr;
                for (const auto& e : cambc::vision::champions(team::enemy)) {
                    if (!e.is_visible() || e.health() <= 0)
                        continue;
                    if (!range.is_set(e.position()))
                        continue;
                    if (!best || e.health() < best->health())
                        best = &e;
                }
                if (best && abil.can_target_enemies()) {
                    if (abil.use(*best))
                        return;
                }
            }
        } catch (...) {
        }
    }
}

void farm_camps(int lane) {
    for (const auto& camp : cambc::vision::camps())
        if (attack_if_in_range(camp))
            return;

    // Lane split: prefer groups with (index % team_size == lane), else nearest.
    cambc::bitboard goals;
    goals.clear();
    cambc::bitboard preferred;
    preferred.clear();
    const auto visible = cambc::vision::visible_tiles();
    const auto groups = cambc::map::camp_groups();
    const int team_size = static_cast<int>(cambc::champions(team::ally).size());
    const int lane_mod = team_size > 0 ? (lane % team_size) : 0;
    int gi = 0;
    for (const auto& group : groups) {
        const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
        if (group_visible
            && std::holds_alternative<cambc::map::camp_group_respawning>(group.state())) {
            ++gi;
            continue;
        }
        goals |= group.spawn_tiles();
        if (team_size > 0 && (gi % team_size) == lane_mod)
            preferred |= group.spawn_tiles();
        ++gi;
    }
    if (!preferred.is_empty())
        goals = preferred;

    if (move_towards_closest(goals)) {
        for (const auto& camp : cambc::vision::camps())
            if (attack_if_in_range(camp))
                return;
    }
}

void maybe_ward() {
    auto& me = cambc::self();
    try {
        if (me.ward_cooldown() != 0)
            return;
        if (!me.ward_positions().empty())
            return;  // one active ward; keep it
        const auto placement = me.ward_placement_tiles();
        if (placement.is_empty())
            return;
        // Deterministic target: nearest altar, else nearest visible camp.
        vec2 anchor = me.position();
        int best = std::numeric_limits<int>::max();
        for (auto altar : cambc::map::altars().positions()) {
            int d = cheb(me.position(), altar);
            if (d < best) {
                best = d;
                anchor = altar;
            }
        }
        if (best == std::numeric_limits<int>::max()) {
            for (const auto& c : cambc::vision::camps()) {
                int d = cheb(me.position(), c.position());
                if (d < best) {
                    best = d;
                    anchor = c.position();
                }
            }
        }
        // Step from us toward anchor, clamped to placement range (<=2).
        for (auto tile : placement.positions()) {
            if (cheb(tile, anchor) < cheb(me.position(), anchor)) {
                me.place_ward(tile);
                return;
            }
        }
    } catch (...) {
    }
}

}  // namespace

void Bot::init() {
    auto& me = cambc::self();
    build_item = build_for(me.champion_name());
    lane = my_lane();
    initialized = true;
}

void Bot::buy_items(bool at_spawn) {
    auto& me = cambc::self();
    // Tavi shops shards anywhere; everyone shops on spawn / while dead.
    const bool is_tavi = me.champion_name() == cambc::champion_type::tavi;
    if (at_spawn || !me.is_alive() || is_tavi) {
        try {
            const unsigned count = me.gold_count() / build_item.cost();
            if (count && me.can_buy(build_item, count))
                me.buy(build_item, count);
        } catch (...) {
        }
    }
    // Consumables: one lock + one crystal when affordable. Spawn/dead only
    // (Tavi also needs spawn for consumables).
    if (at_spawn || !me.is_alive()) {
        try {
            auto& inv = me.consumables();
            if (inv[cambc::consumable_type::altar_lock].count() == 0
                && me.can_buy(cambc::consumable_type::altar_lock))
                me.buy(cambc::consumable_type::altar_lock);
        } catch (...) {
        }
        try {
            auto& inv = me.consumables();
            if (inv[cambc::consumable_type::tp_crystal].count() == 0
                && me.can_buy(cambc::consumable_type::tp_crystal))
                me.buy(cambc::consumable_type::tp_crystal);
        } catch (...) {
        }
    }
}

void Bot::play_turn() {
    if (cambc::fuel_remaining() < 2000) {
        cambc::end_turn();
        return;
    }
    if (!initialized)
        init();

    // Shared memory protocol: bytes 0..3 round (u32 LE, as starter),
    // byte 4+lane status (0 farm, 1 tribute, 2 hurt). Lanes don't collide.
    const int lane_id = my_lane();
    lane = lane_id;
    auto& me = cambc::self();

    // Leave channels running; dead champions only shop.
    if (!me.is_alive()) {
        buy_items(false);
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting())
        return;
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) {
        buy_items(false);
        return;
    }

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    buy_items(at_spawn);

    // Flee with crystal when critical and hunted: 3 tiles toward spawn.
    if (!at_spawn && me.health() < me.max_health() * 0.25f
        && !cambc::vision::champions(team::enemy).empty()) {
        try {
            auto& inv = me.consumables();
            if (inv[cambc::consumable_type::tp_crystal].count() > 0) {
                vec2 spawn = cambc::map::spawn_region(team::ally).positions()[0];
                vec2 dest = me.position();
                int best = cheb(me.position(), spawn);
                for (int dx = -3; dx <= 3; ++dx)
                    for (int dy = -3; dy <= 3; ++dy) {
                        if (std::max(std::abs(dx), std::abs(dy)) > 3)
                            continue;
                        vec2 t{me.position().x + dx, me.position().y + dy};
                        if (!cambc::map::is_in_bounds(t))
                            continue;
                        if (cheb(t, spawn) < best) {
                            best = cheb(t, spawn);
                            dest = t;
                        }
                    }
                if (dest != me.position() && inv[cambc::consumable_type::tp_crystal].use(dest))
                    return;
            }
        } catch (...) {
        }
    }

    // Recall when badly hurt away from spawn.
    if (!at_spawn && me.health() < me.max_health() * 0.35f) {
        try {
            me.recall();
        } catch (...) {
        }
        return;
    }

    // Attack enemy champion already in basic range.
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (attack_if_in_range(enemy)) {
            try_support_abilities();
            me.set_priority(priority_for(me.champion_name(), me.gem_count() > 0));
            return;
        }

    // Tribute carried gems: lock our altar first, then channel.
    if (me.gem_count() > 0) {
        for (auto altar : cambc::map::altars().positions()) {
            if (cheb(me.position(), altar) > 2)
                continue;
            if (cambc::vision::has_altar_lock(altar))
                continue;  // enemy lock: attack converts automatically; skip to farm logic below
            // Place our lock if we carry one and altar is in range.
            try {
                auto& inv = me.consumables();
                if (inv[cambc::consumable_type::altar_lock].count() > 0)
                    inv[cambc::consumable_type::altar_lock].use(altar);
            } catch (...) {
            }
            if (cheb(me.position(), altar) <= cambc::constants::submit_gems_range) {
                try {
                    me.submit_gems(altar);
                } catch (...) {
                    // Locked or busy: fall through to combat/farm.
                    break;
                }
                return;
            }
        }
        // Walk to nearest unlocked altar approach.
        cambc::bitboard goals;
        goals.clear();
        for (auto altar : cambc::map::altars().positions()) {
            if (!cambc::vision::has_altar_lock(altar))
                goals |= tiles_around(altar, cambc::constants::submit_gems_range);
        }
        if (!goals.is_empty()) {
            move_towards_closest(goals);
            // Fresh arrival may now be in submit range.
            for (auto altar : cambc::map::altars().positions()) {
                if (cheb(me.position(), altar) <= cambc::constants::submit_gems_range
                    && !cambc::vision::has_altar_lock(altar)) {
                    try {
                        me.submit_gems(altar);
                    } catch (...) {
                    }
                    break;
                }
            }
            try_support_abilities();
            me.set_priority(priority_for(me.champion_name(), true));
            return;
        }
        // All altars locked: clear nearest enemy lock with basics.
        for (const auto& lock : cambc::vision::altar_locks()) {
            try {
                if (lock.is_enemy() && me.attack_tiles().is_set(lock.position())) {
                    me.attack(lock);
                    return;
                }
            } catch (...) {
            }
        }
    }

    // Excluding-movement nuke before stepping (else the step blocks it).
    bool cast_exclusive = try_nuke();

    // Basic attack on camps in range (after nuke softens them).
    for (const auto& camp : cambc::vision::camps())
        if (attack_if_in_range(camp)) {
            try_support_abilities();
            maybe_ward();
            me.set_priority(priority_for(me.champion_name(), me.gem_count() > 0));
            return;
        }

    // Move toward lane-assigned camp (unless we just cast an exclusive spell).
    if (!cast_exclusive)
        farm_camps(lane);
    try_support_abilities();
    maybe_ward();
    me.set_priority(priority_for(me.champion_name(), me.gem_count() > 0));

    if (cambc::fuel_remaining() < 1000)
        cambc::end_turn();
}
