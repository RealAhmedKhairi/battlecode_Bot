#pragma once
#include <cambc.h>

// Deterministic per-champion state. No randomness: build and lane derive
// from champion type and team roster order.
class Bot {
    bool initialized = false;
    cambc::item_type build_item = cambc::item_type::power_shard_of_strength;
    int lane = 0;

    void init();
    void buy_items(bool at_spawn);

public:
    void play_turn();
};
