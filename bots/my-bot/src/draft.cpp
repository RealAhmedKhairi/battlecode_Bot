#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <vector>

// Fixed preference order: economy + fighters first, fragile/complex last.
// Deterministic: pick the first preference our team has not taken yet.
// Mirroring the opponent is allowed, so only skip our own picks.
void play_draft() {
    constexpr std::array<cambc::champion_draft, 11> preference{
        cambc::champion_draft::vesper_vale,      // gold bonus + strong basics
        cambc::champion_draft::vendetta,         // kill resets, great farmer
        cambc::champion_draft::ren,              // stun + dash
        cambc::champion_draft::varg,             // splash + shield
        cambc::champion_draft::ryth,             // tanky area stun
        cambc::champion_draft::stellar,          // reliable spell damage
        cambc::champion_draft::marcella,         // stun immune frontline
        cambc::champion_draft::tavi,             // field shop + big stun
        cambc::champion_draft::prism,            // gem scaling
        cambc::champion_draft::willow,           // healer
        cambc::champion_draft::cassi_and_issac,  // two bodies, shared death: last
    };

    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    for (auto pick : preference) {
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            // Draft memory: version tag + pick order index for lanes.
            // Bytes: "mybot-v1" + ally pick count. Read-only in game.
            std::string tag = "mybot-v1:";
            tag.push_back(static_cast<char>('0' + chosen.size()));
            cambc::draft_memory = std::string_view(tag);
            cambc::draft::pick(pick);
            return;
        }
    }
}
