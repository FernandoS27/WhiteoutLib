// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// The name source (EDIT_MODE_AUTO_IK_DESIGN.md §3.1, §3.2): what a node's name
// says it is, one table per naming scheme, measured on the corpus.
//
//   Warcraft III HD   L_upr_arm, L_lwr_arm, L_lwr_arm_twist_01, bone_hand_left,
//                     L_leg_01..03, bone_leg_left, L_kneecap, pelvis, bone_turret,
//                     hd_anim (the face root), mount_ for a mount's rig, whose
//                     front legs are arm_01..04 into bone_hand
//   Warcraft III SD   UpArmL, LowArmL, HandL, UpperLegL, LowerLegL, FootL,
//                     ShoulderL, Bone_Chest, Root
//   3ds Max Biped     Bip01 L UpperArm, Bip01 L Forearm, Bip01 L Thigh, Bip01 L Calf
//   Diablo III        left_thigh, right_knee
//   StarCraft II      bone_arm1_l, bone_armfore_l, uppercalf_left
//
// Words are split as the mirror map splits a name — at separators and case
// changes — and at digits, then read against the tables. Exclusions come first,
// so `L_hand_ribbon_01` is not a hand.

#include "detect_parts.h"

#include <whiteout/models/wem/skinning/mirror.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {
namespace rig_detect {

namespace {

/// @p name's words, lower-cased: split at `_`, space, `.` and `-`, at a lower
/// letter followed by an upper one, and where letters and digits meet.
std::vector<std::string> Words(const std::string& name) {
    std::vector<std::string> out;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            out.push_back(word);
            word.clear();
        }
    };
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c == '_' || c == ' ' || c == '.' || c == '-') {
            flush();
            continue;
        }
        if (!word.empty() && i > 0) {
            const unsigned char prev = static_cast<unsigned char>(name[i - 1]);
            const bool caseChange = std::isupper(c) && std::islower(prev);
            const bool digitEdge = (std::isdigit(c) != 0) != (std::isdigit(prev) != 0);
            if (caseChange || digitEdge) {
                flush();
            }
        }
        word.push_back(static_cast<char>(std::tolower(c)));
    }
    flush();
    return out;
}

bool IsNumber(const std::string& word) {
    return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
}

bool Has(const std::vector<std::string>& words, const char* word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

/// A word that contains @p part anywhere.
bool Contains(const std::vector<std::string>& words, const char* part) {
    for (const std::string& word : words) {
        if (word.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool AnyOf(const std::vector<std::string>& words, std::initializer_list<const char*> list) {
    for (const char* word : list) {
        if (Has(words, word)) {
            return true;
        }
    }
    return false;
}

/// @p pattern's words in order, one after another in @p words, or glued into
/// one word (`upr arm` matches `upr_arm`, `UprArm` and `uprarm`).
bool Matches(const std::vector<std::string>& words, std::initializer_list<const char*> pattern) {
    const std::vector<std::string> parts(pattern.begin(), pattern.end());
    std::string glued;
    for (const std::string& part : parts) {
        glued += part;
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (words[i] == glued) {
            return true;
        }
        if (i + parts.size() > words.size()) {
            continue;
        }
        bool all = true;
        for (std::size_t k = 0; k < parts.size() && all; ++k) {
            all = words[i + k] == parts[k];
        }
        if (all) {
            return true;
        }
    }
    return false;
}

bool MatchesAny(const std::vector<std::string>& words,
                std::initializer_list<std::initializer_list<const char*>> patterns) {
    for (const auto& pattern : patterns) {
        if (Matches(words, pattern)) {
            return true;
        }
    }
    return false;
}

/// Limb words a side letter is sometimes run onto with no separator, in
/// all-lower-case custom rigs (`uparml`, `handr`): the only place a bare
/// suffix is read as a side, because `control` also ends in an `l`.
constexpr const char* kGluedSides[] = {"hand",     "uparm",   "lowarm",  "upperleg", "lowerleg",
                                       "foot",     "thigh",   "calf",    "upperarm", "forearm",
                                       "shoulder", "clavicle", "upleg",  "lowleg"};

} // namespace

NameRead ReadName(const std::string& name) {
    NameRead read;
    std::vector<std::string> words = Words(name);

    // What every scheme wraps a name in: HD's `_bind_jnt`, a Biped's `Bip01`,
    // a mount's `mount_`. A Biped's bare `Bip01` IS its centre of mass.
    while (!words.empty() && (words.back() == "jnt" || words.back() == "bind")) {
        words.pop_back();
    }
    if (words.size() >= 2 && words[0] == "bip" && IsNumber(words[1])) {
        if (words.size() == 2) {
            read.body = true;
            read.role = RigRole::Body;
            return read;
        }
        words.erase(words.begin(), words.begin() + 2);
    }
    if (!words.empty() && words[0] == "mount") {
        read.mount = true;
        words.erase(words.begin());
    }

    // The side, as the mirror map reads it; then the side words themselves go.
    switch (skinning::SideOfName(name)) {
    case skinning::NameSide::Left:
        read.side = RigSide::Left;
        break;
    case skinning::NameSide::Right:
        read.side = RigSide::Right;
        break;
    case skinning::NameSide::None:
        break;
    }
    std::erase_if(words, [](const std::string& word) {
        return word == "l" || word == "r" || word == "left" || word == "right" || word == "lf" ||
               word == "rt";
    });
    if (read.side == RigSide::Centre && !words.empty()) {
        std::string& last = words.back();
        for (const char* glued : kGluedSides) {
            const std::string stem = glued;
            if (last.size() == stem.size() + 1 && last.compare(0, stem.size(), stem) == 0 &&
                (last.back() == 'l' || last.back() == 'r')) {
                read.side = last.back() == 'l' ? RigSide::Left : RigSide::Right;
                last.pop_back();
                break;
            }
        }
    }
    std::erase_if(words, [](const std::string& word) { return word == "bone"; });
    if (words.empty()) {
        return read;
    }

    // Exclusions first: a ribbon or an effect riding a hand is not a hand, and
    // cloth, straps and hair hang off a limb without being one.
    if (AnyOf(words, {"ribbon", "fx", "sfx", "vfx", "cloth", "loincloth", "tassel", "rein", "stirrup",
                      "mane", "cape", "hair", "skirt", "strap", "stretch", "nub"})) {
        return read;
    }
    if (Matches(words, {"hd", "anim"})) {
        read.face = true;
        return read;
    }

    const bool armish = MatchesAny(words, {{"arm"}, {"forearm"}, {"uparm"}, {"lowarm"},
                                           {"upperarm"}, {"lowerarm"}, {"hand"}, {"elbow"}});
    const bool legish = MatchesAny(words, {{"leg"}, {"thigh"}, {"calf"}, {"shin"}, {"knee"},
                                           {"foot"}, {"upleg"}, {"lowleg"}, {"foreleg"}, {"hindleg"}});
    const RigLimb limb = armish ? RigLimb::Arm : legish ? RigLimb::Leg : RigLimb::Other;

    if (Contains(words, "twist") || Has(words, "roll")) {
        read.role = RigRole::Twist;
        read.limb = limb;
        return read;
    }
    if (Contains(words, "finger") || AnyOf(words, {"tmb", "thumb", "pinky", "digit", "claw"})) {
        return read;
    }
    if (Contains(words, "toe")) {
        read.role = RigRole::Toe;
        read.limb = RigLimb::Leg;
        return read;
    }
    if (AnyOf(words, {"kneecap", "kneepad", "armpad", "elbowpad", "pauldron", "shoulderpad", "pad"}) ||
        Contains(words, "pauldron") || Contains(words, "kneecap")) {
        read.role = RigRole::Pad;
        read.limb = AnyOf(words, {"kneecap", "kneepad"}) || Contains(words, "kneecap") ? RigLimb::Leg
                                                                                       : RigLimb::Arm;
        return read;
    }
    if (AnyOf(words, {"weapon", "shield", "sword", "book", "axe", "hammer", "mace", "staff", "spear",
                      "bow", "gun", "rifle", "blade", "lance", "dagger", "polearm", "club", "wand",
                      "scythe", "halberd", "banner", "torch"})) {
        read.role = RigRole::Prop;
        return read;
    }

    // Numbered joints: HD's `leg_01..03`, a mount's `arm_01..04`, the dryads'
    // `foreleg_01..04`, SC2's `arm1`. One is the Upper and two the Lower; from
    // three on the joint is a Hock above the End and a Toe below it, which only
    // its place can say.
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        const bool legWord = words[i] == "leg" || words[i] == "foreleg" || words[i] == "hindleg";
        if ((words[i] == "arm" || legWord) && IsNumber(words[i + 1])) {
            const u32 index = static_cast<u32>(std::strtoul(words[i + 1].c_str(), nullptr, 10));
            // A number followed by another number or by `end` is a figure
            // index, not a place along the limb: Deluxe Edition writes the
            // side, the figure, then the joint (`leg_L1_0_jnt`, `arm_L1_end_jnt`
            // on a mount), and read as HD numbering a whole leg came out Upper.
            const bool figure =
                i + 2 < words.size() && (IsNumber(words[i + 2]) || words[i + 2] == "end");
            if (index == 0 || figure) {
                break;
            }
            read.index = index;
            read.limb = legWord ? RigLimb::Leg : RigLimb::Arm;
            read.role = index == 1 ? RigRole::Upper : index == 2 ? RigRole::Lower : RigRole::Hock;
            return read;
        }
    }

    // A chain terminator, where a Maya export puts one: `leg_L0_end_jnt`,
    // `arm_R0_end_jnt`, `legHind_L0_end_jnt`. It sits on the ankle or the
    // wrist, and what hangs below it (`foot_L0_0_jnt`, the ball of the foot;
    // `meta_L0_0_jnt`, the palm) is past where the limb ends. Warcraft III
    // Deluxe Edition names every limb this way, and its figure index (the
    // `0` in `leg_L0_end`) breaks the numbered loop above, so read as
    // nothing the End fell to the ball and the chain came out one joint
    // low: knee, ankle, ball. Measured on 231 of 773 DE units.
    if (Has(words, "end") && limb != RigLimb::Other) {
        read.role = RigRole::End;
        read.limb = limb;
        read.terminator = true;
        return read;
    }
    if (MatchesAny(words, {{"upr", "arm"}, {"up", "arm"}, {"upper", "arm"}, {"bicep"}})) {
        read.role = RigRole::Upper;
        read.limb = RigLimb::Arm;
    } else if (MatchesAny(words, {{"lwr", "arm"}, {"low", "arm"}, {"lower", "arm"}, {"fore", "arm"},
                                  {"arm", "fore"}})) {
        read.role = RigRole::Lower;
        read.limb = RigLimb::Arm;
    } else if (AnyOf(words, {"hand", "palm", "wrist"})) {
        read.role = RigRole::End;
        read.limb = RigLimb::Arm;
    } else if (MatchesAny(words, {{"thigh"}, {"upper", "leg"}, {"up", "leg"}, {"upr", "leg"}})) {
        read.role = RigRole::Upper;
        read.limb = RigLimb::Leg;
    } else if (MatchesAny(words, {{"calf"}, {"shin"}, {"lower", "leg"}, {"low", "leg"},
                                  {"lwr", "leg"}, {"knee"}, {"upper", "calf"}})) {
        read.role = RigRole::Lower;
        read.limb = RigLimb::Leg;
    } else if (AnyOf(words, {"foot", "ankle", "paw", "hoof"}) ||
               (words.size() == 1 && words[0] == "leg")) {
        // HD's foot is `bone_leg_left`: the word `leg` and nothing else.
        read.role = RigRole::End;
        read.limb = RigLimb::Leg;
    } else if (AnyOf(words, {"clavicle", "collar", "shoulder"})) {
        read.role = RigRole::Clavicle;
        read.limb = RigLimb::Arm;
    } else if (Has(words, "neck")) {
        read.role = RigRole::Neck;
    } else if (Has(words, "head")) {
        read.role = RigRole::Head;
    } else if (AnyOf(words, {"pelvis", "turret", "root", "com", "cog", "hips"})) {
        read.body = true;
        read.stage = words.size() == 1 && words[0] == "root";
        read.role = RigRole::Body;
    } else if (Contains(words, "spine") ||
               AnyOf(words, {"chest", "torso", "abdomen", "belly", "ribcage", "waist"})) {
        read.role = RigRole::Spine;
    }
    return read;
}

void NameTier(Work& work) {
    const u32 count = work.tree.size();
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsJoint(n)) {
            continue;
        }
        const NameRead& read = work.names[n];
        if (read.role == RigRole::None) {
            continue;
        }
        // A body word is the Body for now: which tops are figures is only known
        // once the limbs are (`BodyTier`), and anything else becomes spine then.
        work.Set(n, read.role, read.side, read.limb, RigSource::Name);
    }
}

void NumberedTier(Work& work) {
    // Deepest first: whether `leg_03` is a Hock depends on `leg_04` below it
    // having become the End, and an imported file may list `leg_04` before its
    // parents (the HD centaur archer does).
    std::vector<std::pair<u32, u32>> numbered; // depth, node
    for (u32 n = 0; n < work.tree.size(); ++n) {
        if (work.names[n].index >= 3 && work.rig[n].source == RigSource::Name &&
            work.rig[n].role == RigRole::Hock) {
            numbered.emplace_back(work.tree.depth(n), n);
        }
    }
    std::stable_sort(numbered.begin(), numbered.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [depth, n] : numbered) {
        bool pastEnd = false;
        for (u32 up = work.Parent(n); up != kInvalidNode; up = work.Parent(up)) {
            if (work.rig[up].role == RigRole::End) {
                pastEnd = true;
                break;
            }
        }
        bool aboveEnd = false;
        bool lastNumbered = true;
        for (const u32 below : work.tree.subtree(n)) {
            if (below == n) {
                continue;
            }
            aboveEnd = aboveEnd || work.rig[below].role == RigRole::End;
            lastNumbered = lastNumbered && work.names[below].index == 0;
        }
        NodeRig& rig = work.rig[n];
        if (pastEnd) {
            // The footman's `leg_04` hangs below its foot.
            if (rig.limb == RigLimb::Leg) {
                rig.role = RigRole::Toe;
                continue;
            }
        } else if (aboveEnd) {
            continue; // A Hock.
        } else if (lastNumbered) {
            // The last of `leg_01..04` with nothing past it that ends the limb
            // is where the limb ends.
            rig.role = RigRole::End;
            continue;
        }
        rig.role = RigRole::None;
        rig.side = RigSide::Centre;
        rig.limb = RigLimb::Other;
        rig.source = RigSource::None;
    }
}

void LineTier(Work& work) {
    const auto guessed = [&](u32 n) {
        return work.rig[n].source != RigSource::File && work.rig[n].source != RigSource::You;
    };
    // Where a limb's line starts: nothing above it is this limb's.
    const auto top = [&](u32 n) {
        const RigRole role = work.rig[n].role;
        return role == RigRole::Upper || role == RigRole::Clavicle || role == RigRole::Body ||
               role == RigRole::Spine || role == RigRole::Neck || role == RigRole::Head;
    };
    const u32 count = work.tree.size();
    // An End with another End above it in the line is past that one.
    std::vector<u32> past;
    for (u32 n = 0; n < count; ++n) {
        if (work.rig[n].role != RigRole::End || !guessed(n)) {
            continue;
        }
        for (u32 up = work.Parent(n); up != kInvalidNode && !top(up); up = work.Parent(up)) {
            if (work.rig[up].role == RigRole::End) {
                past.push_back(n);
                break;
            }
        }
    }
    for (const u32 n : past) {
        work.rig[n].role = RigRole::Toe;
    }
    // Of the Lowers in an End's line, the top one bends as the knee or the
    // elbow; any below it bend as the Hock.
    for (u32 n = 0; n < count; ++n) {
        if (work.rig[n].role != RigRole::End) {
            continue;
        }
        std::vector<u32> lowers; // End first
        for (u32 up = work.Parent(n); up != kInvalidNode && !top(up); up = work.Parent(up)) {
            if (work.rig[up].role == RigRole::Lower) {
                lowers.push_back(up);
            }
        }
        for (std::size_t i = 0; i + 1 < lowers.size(); ++i) {
            if (guessed(lowers[i])) {
                work.rig[lowers[i]].role = RigRole::Hock;
            }
        }
    }
}

} // namespace rig_detect
} // namespace wem
} // namespace models
} // namespace whiteout
