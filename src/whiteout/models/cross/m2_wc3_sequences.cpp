// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/m2_wc3_sequences.h"

#include <set>
#include <string>
#include <vector>

#include "whiteout/models/m2/animation_names.h"

namespace whiteout {
namespace models {
namespace cross {

namespace {

struct Row {
    const char* wow; ///< The `AnimationData` name, as `m2::animationName` spells it.
    const char* wc3;
    bool nonLooping;
};

// Warcraft III's tags stand in for what it has no word of its own for:
// `First`..`Fifth` name the weapon (one-handed, two-handed, two-handed loose,
// bow, crossbow or gun), `Gold` and `Lumber` a ranged weapon readied and held,
// and `Cinematic` keeps an animation out of the game's own choosing.
// clang-format off
constexpr Row kRows[] = {
    {"ArtBowLoop",                   "Cinematic Art Bow Loop",                    false},
    {"ArtDualLoop",                  "Cinematic Art Dual Loop",                   false},
    {"ArtFistsLoop",                 "Cinematic Art Fists Loop",                  false},
    {"ArtMainLoop",                  "Cinematic Art Main Loop",                   false},
    {"ArtOffLoop",                   "Cinematic Art Off Loop",                    false},
    {"ArtShieldLoop",                "Cinematic Art Shield Loop",                 false},
    {"Attack1H",                     "Attack First",                              true},
    {"Attack1HPierce",               "Attack First",                              true},
    {"Attack2H",                     "Attack Second",                             true},
    {"Attack2HL",                    "Attack Second",                             true},
    {"AttackBow",                    "Attack Fourth",                             true},
    {"AttackCrossbow",               "Attack Fifth",                              true},
    {"AttackDart",                   "Attack Poke",                               true},
    {"AttackFist1H",                 "Attack",                                    true},
    {"AttackFist1HOff",              "Attack",                                    true},
    {"AttackFL",                     "Attack First",                              true},
    {"AttackFLOff",                  "Attack First",                              true},
    {"AttackJoust",                  "Cinematic MountAttack",                     true},
    {"AttackOff",                    "Attack Off",                                true},
    {"AttackOffPierce",              "Attack",                                    true},
    {"AttackRifle",                  "Attack Fifth",                              true},
    {"AttackThrown",                 "Attack Puke",                               true},
    {"AttackUnarmed",                "Attack",                                    true},
    {"BarServerEmoteTalk",           "Cinematic BarServerTalk",                   false},
    {"BarServerStand",               "Cinematic BarServerStand",                  false},
    {"BarServerWalk",                "Cinematic BarServerWalk",                   false},
    {"BarTendEmotePoint",            "Cinematic BarTendPoint",                    true},
    {"BarTendEmoteTalk",             "Cinematic BarTendTalk",                     false},
    {"BarTendStand",                 "Cinematic BarTendStand",                    false},
    {"BattleRoar",                   "Spell Slam",                                true},
    {"BreathOfFire",                 "Cinematic Monk BreathOfFire",               true},
    {"CarriageMount",                "Cinematic CarriageMountStand",              false},
    {"CarriageMountAttack",          "Cinematic CarriageMountAttack",             true},
    {"Carried2H",                    "Cinematic Carried",                         false},
    {"Carry2H",                      "Cinematic Carry",                           false},
    {"CastCurseLeft",                "Cinematic CastCurseLeft",                   true},
    {"CastCurseRight",               "Cinematic CastCurseRight",                  true},
    {"CastOutStrong",                "Cinematic CastOutStrong",                   true},
    {"CastStrongLeft",               "Cinematic CastStrongLeft",                  true},
    {"CastStrongRight",              "Cinematic CastStrongRight",                 true},
    {"CastStrongUpLeft",             "Cinematic CastStrongUpLeft",                true},
    {"CastStrongUpRight",            "Cinematic CastStrongUpRight",               true},
    {"CastSweepLeft",                "Cinematic CastSweepLeft",                   true},
    {"CastSweepRight",               "Cinematic CastSweepRight",                  true},
    {"CastTwistUpBoth",              "Cinematic CastTwistUpBoth",                 true},
    {"ChannelCastDirected",          "Stand Channel Throw",                       false},
    {"ChannelCastOmni",              "Spell Ready 2",                             false},
    {"ChannelCastOmniUp",            "Stand Channel 2",                           false},
    {"CombatAbility1H01",            "Cinematic CombatAbility1H01",               true},
    {"CombatAbility1H01Off",         "Cinematic CombatAbility1H01Off",            true},
    {"CombatAbility1H02",            "Cinematic CombatAbility1H02",               true},
    {"CombatAbility1H02Off",         "Cinematic CombatAbility1H02Off",            true},
    {"CombatAbility1H03Off",         "Cinematic CombatAbility1H03Off",            true},
    {"CombatAbility1HBig01",         "Cinematic CombatAbility1HBig01",            true},
    {"CombatAbility1HOffPierce",     "Cinematic CombatAbility1HOffPierce",        true},
    {"CombatAbility1HPierce",        "Cinematic CombatAbility1HPierce",           true},
    {"CombatAbility2H01",            "Cinematic CombatAbility2H01",               true},
    {"CombatAbility2H02",            "Cinematic CombatAbility2H02",               true},
    {"CombatAbility2H03",            "Cinematic CombatAbility2H03",               true},
    {"CombatAbility2HBig01",         "Cinematic CombatAbility2HBig01",            true},
    {"CombatAbility2HBig02",         "Cinematic CombatAbility2HBig02",            true},
    {"CombatAbilityDualWield01",     "Cinematic CombatAbilityDualWield01",        true},
    {"CombatBladeStorm",             "Cinematic BladeStorm",                      true},
    {"CombatChargeEnd",              "Cinematic CombatChargeEnd",                 true},
    {"CombatChargeLoop",             "Cinematic CombatChargeLoop",                false},
    {"CombatCritical",               "Stand Hit Critical",                        false},
    {"CombatEviscerate",             "Cinematic CombatEviscerate",                false},
    {"CombatExecute",                "Cinematic CombatExecute",                   true},
    {"CombatFinishingMove",          "Cinematic CombatFinishingMove",             true},
    {"CombatFuriousStrikes",         "Cinematic CombatFuriousStrikes",            true},
    {"CombatLeapEnd",                "Cinematic CombatLeapEnd",                   true},
    {"CombatLeapStart",              "Cinematic CombatLeapStart",                 true},
    {"CombatMutilate",               "Cinematic CombatMutilate",                  true},
    {"CombatPistolShotOff",          "Cinematic CombatPistolShotOff",             true},
    {"CombatShieldBash",             "Cinematic CombatShieldBash",                true},
    {"CombatShieldThrow",            "Cinematic CombatShieldThrow",               true},
    {"CombatStompLeft",              "Cinematic CombatStompLeft",                 true},
    {"CombatStompRight",             "Cinematic CombatStompRight",                true},
    {"CombatThrow",                  "Cinematic CombatThrow",                     true},
    {"CombatWhirlwind",              "Cinematic CombatWhirlwind",                 true},
    {"CombatWound",                  "Stand Hit Large",                           false},
    {"CookingLoop",                  "Cinematic CookingLoop",                     false},
    {"Cower",                        "Cinematic Cower",                           false},
    {"CustomSpell03",                "Cinematic CustomSpell03",                   false},
    {"CustomSpell04",                "Cinematic CustomSpell04",                   false},
    {"Death",                        "Death",                                     true},
    {"DeathStrike",                  "Cinematic DeathStrike",                     true},
    {"DkCast1HFront",                "Cinematic DkCast1HFront",                   true},
    {"Dodge",                        "Stand Hit Small",                           false},
    {"DragonStomp",                  "Cinematic Dragon Stomp",                    true},
    {"Drown",                        "Death Swim",                                true},
    {"Drowned",                      "Decay Swim",                                false},
    {"DruReadySpellCast",            "Cinematic DruReadySpellCast",               false},
    {"DruSpellCastBothFront",        "Cinematic DruSpellCastBothFront",           true},
    {"DruSpellPrecastBoth",          "Cinematic DruSpellPrecastBoth",             true},
    {"DruSpellPrecastBothChannel",   "Cinematic DruSpellPrecastBothChannel",      false},
    {"EmoteApplaud",                 "Cinematic Applaud",                         true},
    {"EmoteBeg",                     "Cinematic Beg",                             true},
    {"EmoteBow",                     "Cinematic Bow",                             true},
    {"EmoteCheer",                   "Cinematic Cheer",                           true},
    {"EmoteChicken",                 "Cinematic Chicken",                         true},
    {"EmoteCry",                     "Cinematic Cry",                             true},
    {"EmoteDance",                   "Cinematic Dance",                           false},
    {"EmoteDanceSpecial",            "Cinematic Dance Special",                   false},
    {"EmoteEat",                     "Cinematic Eat",                             true},
    {"EmoteEatNoSheathe",            "Cinematic EatNoSheathe",                    true},
    {"EmoteFlex",                    "Cinematic Flex",                            true},
    {"EmoteKiss",                    "Cinematic Kiss",                            true},
    {"EmoteKneel",                   "Cinematic Kneel Special",                   true},
    {"EmoteLaugh",                   "Cinematic Laugh",                           true},
    {"EmoteNo",                      "Cinematic No",                              true},
    {"EmotePoint",                   "Cinematic Point",                           true},
    {"EmotePointNoSheathe",          "Cinematic PointNoSheathe",                  true},
    {"EmoteReadEnd",                 "Cinematic Read End",                        true},
    {"EmoteReadLoop",                "Cinematic Read",                            false},
    {"EmoteReadStart",               "Cinematic Read Start",                      true},
    {"EmoteRoar",                    "Cinematic Roar",                            true},
    {"EmoteRude",                    "Cinematic Rude",                            true},
    {"EmoteSalute",                  "Cinematic Salute",                          true},
    {"EmoteSaluteNoSheathe",         "Cinematic SaluteNoSheathe",                 true},
    {"EmoteShout",                   "Cinematic Shout",                           true},
    {"EmoteShy",                     "Cinematic Shy",                             true},
    {"EmoteStunNoSheathe",           "Cinematic StunNoSheathe",                   false},
    {"EmoteTalk",                    "Portrait Talk",                             false},
    {"EmoteTalkExclamation",         "Portrait Talk 2",                           false},
    {"EmoteTalkNoSheathe",           "Portrait Talk",                             false},
    {"EmoteTalkQuestion",            "Portrait Talk 3",                           false},
    {"EmoteTrain",                   "Cinematic Train",                           true},
    {"EmoteUseStanding",             "Cinematic Use",                             false},
    {"EmoteUseStandingNoSheathe",    "Cinematic Use",                             false},
    {"EmoteWave",                    "Cinematic Wave",                            true},
    {"EmoteWork",                    "Cinematic Work",                            false},
    {"EmoteWorkNoSheathe",           "Cinematic Work",                            false},
    {"EmoteYes",                     "Cinematic Yes",                             true},
    {"FacePose",                     "Cinematic Facepose",                        false},
    {"FalconeerEnd",                 "Cinematic Falconeer End",                   true},
    {"FalconeerLoop",                "Cinematic Falconeer",                       false},
    {"FalconeerStart",               "Cinematic Falconeer Start",                 true},
    {"Fall",                         "Cinematic Fall",                            false},
    {"FishingCast",                  "Cinematic Fishing Start",                   true},
    {"FishingLoop",                  "Cinematic Fishing",                         false},
    {"FlyingKick",                   "Cinematic Monk Flying Kick",                false},
    {"FlyingKickEnd",                "Cinematic Monk Flying Kick End",            true},
    {"FlyingKickStart",              "Cinematic Monk Flying Kick Start",          true},
    {"HandsClosed",                  "Cinematic HandsClosed",                     false},
    {"HerbalismLoop",                "Cinematic Herbalism",                       false},
    {"HipSheath",                    "Cinematic HipSheath",                       true},
    {"HoldBow",                      "Stand Lumber Fourth",                       false},
    {"HoldCrossbow",                 "Stand Lumber Fifth",                        false},
    {"HoldDart",                     "Cinematic HoldDart",                        false},
    {"HoldJoust",                    "Cinematic MountHoldJoust",                  false},
    {"HoldRifle",                    "Stand Lumber Fifth",                        false},
    {"HoldThrown",                   "Stand Ready Alternate",                     false},
    {"Hover",                        "Cinematic Hover",                           false},
    {"Jump",                         "Cinematic Jump",                            false},
    {"JumpEnd",                      "Cinematic Jump End",                        true},
    {"JumpLandRun",                  "Cinematic Jump Land Run",                   true},
    {"JumpStart",                    "Cinematic Jump Start",                      true},
    {"Kick",                         "Cinematic Kick",                            true},
    {"KneelEnd",                     "Cinematic Kneel End",                       true},
    {"KneelLoop",                    "Cinematic Kneel",                           false},
    {"KneelStart",                   "Cinematic Kneel Start",                     true},
    {"Knockdown",                    "Stand Hit Slam",                            true},
    {"LoadBow",                      "Stand Ready Fourth",                        false},
    {"LoadCrossbow",                 "Stand Ready Fifth",                         false},
    {"LoadDart",                     "Cinematic Load Dart",                       true},
    {"LoadJoust",                    "Cinematic Load Joust",                      true},
    {"LoadRifle",                    "Stand Ready Fifth",                         false},
    {"LoadThrown",                   "Stand Ready Alternate",                     false},
    {"LocReadySpellCast",            "Cinematic Warlock Ready Spell Cast",        false},
    {"LocSpellCastBothFront",        "Cinematic Warlock Spell Cast Front",        true},
    {"LocSpellPrecastBoth",          "Cinematic Warlock Spell Both",              true},
    {"LocSpellPrecastBothChannel",   "Cinematic Warlock Spell Channel",           false},
    {"LocSummon",                    "Cinematic Warlock Summon",                  true},
    {"Loot",                         "Cinematic Loot",                            true},
    {"MagReadySpellCast",            "Cinematic Mage Ready Spell Cast",           false},
    {"MagSpellCastBothFront",        "Cinematic Mage Ready Spell Front",          true},
    {"MagSpellPrecastBoth",          "Cinematic Mage Precast",                    true},
    {"MagSpellPrecastBothChannel",   "Cinematic Mage Channel",                    false},
    {"Meditate",                     "Cinematic Monk Meditate",                   false},
    {"Monk2HLIdle",                  "Cinematic Monk Idle",                       false},
    {"MonkDefenseAttackUnarmed",     "Cinematic Monk Defense Attack",             true},
    {"MonkDefenseAttackUnarmedOff",  "Cinematic Monk Defense Attack Off",         true},
    {"MonkDefenseParryUnarmed",      "Cinematic Monk Defense Parry",              true},
    {"MonkDefenseReadyUnarmed",      "Cinematic Monk Defense Ready",              false},
    {"MonkDefenseSpecialUnarmed",    "Cinematic Monk Defense Special",            true},
    {"MonkHealAttackUnarmed",        "Cinematic Monk Attack",                     true},
    {"MonkHealChannelCastDirected",  "Cinematic Monk Channel Cast",               false},
    {"MonkHealChannelCastOmni",      "Cinematic Monk Channel",                    false},
    {"MonkHealParryUnarmed",         "Cinematic Monk Parry",                      true},
    {"MonkHealReadySpellDirected",   "Cinematic Monk Spell Ready Directed",       false},
    {"MonkHealReadySpellOmni",       "Cinematic Monk Spell Ready Directed",       false},
    {"MonkHealReadyUnarmed",         "Cinematic Monk Spell Ready Directed",       false},
    {"MonkHealSpellCastDirected",    "Cinematic Monk Spell Throw",                true},
    {"MonkHealSpellCastOmni",        "Cinematic Monk Spell",                      true},
    {"MonkOffenseAttackUnarmed",     "Cinematic Monk Offense Attack",             true},
    {"MonkOffenseAttackUnarmedOff",  "Cinematic Monk Offense Attack Off",         true},
    {"MonkOffenseAttackWeapon",      "Cinematic Monk Offense Attack Weapon",      true},
    {"MonkOffenseParryUnarmed",      "Cinematic Monk Offense Parry",              true},
    {"MonkOffenseReadyUnarmed",      "Cinematic Monk Offense Ready",              false},
    {"MonkOffenseSpecialUnarmed",    "Cinematic Monk Offense Special",            true},
    {"Mount",                        "Cinematic Mount Stand",                     false},
    {"MountChopper",                 "Cinematic Mount Stand Chopper",             false},
    {"MountExtraWide",               "Cinematic Mount Stand Wide Extra",          false},
    {"MountSelfIdle",                "Cinematic Worgen Mount Stand",              false},
    {"MountSelfStart",               "Cinematic Worgen Mount Start",              true},
    {"MountWide",                    "Cinematic Mount Stand Wide",                false},
    {"Mutilate",                     "Cinematic Rogue Mutilate",                  true},
    {"PalmStrike",                   "Cinematic Monk PalmStrike",                 true},
    {"PalSpellCast1HUp",             "Cinematic Paladin Cast Up",                 true},
    {"PalSpellCastRightFront",       "Cinematic Paladin Spell Cast Front",        true},
    {"PalSpellPrecastRight",         "Cinematic Paladin Spell Channel Right",     false},
    {"Parry1H",                      "Stand Hit Medium First",                    false},
    {"Parry2H",                      "Stand Hit Medium Second",                   false},
    {"Parry2HL",                     "Stand Hit Medium Third",                    false},
    {"ParryFist1H",                  "Stand Hit Medium",                          false},
    {"ParryFL",                      "Stand Hit Shield",                          false},
    {"ParryUnarmed",                 "Stand Hit Medium",                          false},
    {"PriHoverBackward",             "Cinematic Priest Hover Backward",           false},
    {"PriHoverForward",              "Cinematic Priest Hover Forward",            false},
    {"PriHoverLeft",                 "Cinematic Priest Hover Left",               false},
    {"PriHoverRight",                "Cinematic Priest Hover Right",              false},
    {"PriReadyPostSpellCast",        "Cinematic Priest Spell Channel",            false},
    {"PriReadySpellCast",            "Cinematic Priest Spell Ready",              false},
    {"PriSpellCastBothFront",        "Cinematic Priest Spell Cast Front",         true},
    {"PriSpellCastBothFrontChannel", "Cinematic Priest Spell Channel Front",      false},
    {"PriSpellCastBothUp",           "Cinematic Priest Spell Cast Up",            true},
    {"PriSpellCastBothUpChannel",    "Cinematic Priest Spell Channel Up",         false},
    {"PriSpellPrecastBoth",          "Cinematic Priest Spell Precast",            true},
    {"PriSpellPrecastBothChannel",   "Cinematic Priest Spell Precast Channel",    false},
    {"Ready1H",                      "Stand Ready First",                         false},
    {"Ready2H",                      "Stand Ready Second",                        false},
    {"Ready2HL",                     "Stand Ready Third",                         false},
    {"ReadyAbility",                 "Stand Ready Second 2",                      false},
    {"ReadyBow",                     "Stand Gold Fourth",                         false},
    {"ReadyCrossbow",                "Stand Gold Fifth",                          false},
    {"ReadyFist1H",                  "Stand Ready",                               false},
    {"ReadyFL",                      "Stand Ready",                               false},
    {"ReadyJoust",                   "Cinematic ReadyJoust",                      false},
    {"ReadyRifle",                   "Stand Gold Fifth",                          false},
    {"ReadySpellDirected",           "Spell Ready Throw",                         false},
    {"ReadySpellOmni",               "Spell Ready",                               false},
    {"ReadyThrown",                  "Stand Gold Alternate",                      false},
    {"ReadyUnarmed",                 "Stand Ready",                               false},
    {"ReclinedMount",                "Cinematic Mount Reclined",                  false},
    {"ReclinedMountPassenger",       "Cinematic Mount Reclined Passenger",        false},
    {"RisingSunKick",                "Cinematic Monk RisingSunKick",              true},
    {"Roll",                         "Cinematic Monk Roll",                       false},
    {"RollEnd",                      "Cinematic Monk Roll End",                   true},
    {"RollStart",                    "Cinematic Monk Roll Start",                 true},
    {"RoundHouseKick",               "Cinematic Monk RoundHouseKick",             true},
    {"Run",                          "Walk Fast",                                 false},
    {"RunBackwards",                 "Cinematic Backwalk",                        false},
    {"ShaReadySpellCast",            "Cinematic Shaman Spell Channel",            false},
    {"ShaSpellCastBothFront",        "Cinematic Shaman Spell Cast Front",         true},
    {"ShaSpellPrecastBoth",          "Cinematic Shaman Spell Precast",            true},
    {"ShaSpellPrecastBothChannel",   "Cinematic Shaman Spell Precast Channel",    false},
    {"Sheath",                       "Cinematic Sheath",                          false},
    {"ShieldBash",                   "Cinematic ShieldBash",                      true},
    {"ShieldBlock",                  "Stand Hit Defend",                          true},
    {"ShuffleLeft",                  "Cinematic Shuffle Left",                    true},
    {"ShuffleRight",                 "Cinematic Shuffle Right",                   true},
    {"SitChairHigh",                 "Cinematic SitChairHigh",                    false},
    {"SitChairLow",                  "Cinematic SitChairLow",                     false},
    {"SitChairMed",                  "Cinematic SitChairMedium",                  false},
    {"SitGround",                    "Cinematic Sit Ground",                      false},
    {"SitGroundDown",                "Cinematic Sit Ground Start",                true},
    {"SitGroundUp",                  "Cinematic Sit Ground End",                  true},
    {"SkinningLoop",                 "Cinematic Skinning",                        true},
    {"Slam",                         "Cinematic Slam",                            true},
    {"Sleep",                        "Cinematic Sleep",                           false},
    {"SleepDown",                    "Cinematic Sleep Start",                     true},
    {"SleepUp",                      "Cinematic Sleep End",                       true},
    {"Special1H",                    "Attack Slam First",                         true},
    {"Special2H",                    "Attack Slam Second",                        true},
    {"SpecialDual",                  "Attack Slam First",                         true},
    {"SpecialFist1H",                "Attack Slam",                               true},
    {"SpecialFL",                    "Cinematic Special FL",                      true},
    {"SpecialUnarmed",               "Attack Slam",                               true},
    {"SpellCast",                    "Spell",                                     true},
    {"SpellCastDirected",            "Spell Throw",                               true},
    {"SpellCastOmni",                "Spell",                                     true},
    {"SpellKneelEnd",                "Cinematic Spell Kneel End",                 true},
    {"SpellKneelLoop",               "Cinematic Spell Kneel",                     false},
    {"SpellKneelStart",              "Cinematic Spell Kneel Start",               true},
    {"SpellSleepDown",               "Cinematic Spell Sleep Down",                true},
    {"SpinningKick",                 "Cinematic Monk SpinningKick",               false},
    {"Sprint",                       "Walk Fast 2",                               false},
    {"Stand",                        "Stand",                                     false},
    {"StandWound",                   "Stand Hit",                                 false},
    {"StealthRun",                   "Walk Fast Alternate",                       false},
    {"StealthStand",                 "Stand Alternate",                           false},
    {"StealthWalk",                  "Walk Alternate",                            false},
    {"Stop",                         "Cinematic Stop",                            false},
    {"Stormstrike",                  "Cinematic Rogue Stormstrike",               true},
    {"Strangulate",                  "Cinematic Strangulate",                     false},
    {"Stun",                         "Cinematic Stun",                            false},
    {"Swim",                         "Walk Swim",                                 false},
    {"SwimBackwards",                "Cinematic Backswim",                        false},
    {"SwimIdle",                     "Stand Swim",                                false},
    {"SwimLeft",                     "Cinematic Swim Left",                       false},
    {"SwimRight",                    "Cinematic Swim Right",                      false},
    {"SwimRun",                      "Cinematic Swim Run",                        false},
    {"SwimWalk",                     "Cinematic Swim Walk",                       false},
    {"ThousandFists",                "Cinematic Monk Thousand Fists",             false},
    {"ToAltered",                    "Cinematic Worgen Transform",                true},
    {"Torpedo",                      "Cinematic Monk Torpedo",                    true},
    {"UseStandingEnd",               "Cinematic Use End",                         true},
    {"UseStandingLoop",              "Cinematic Use",                             false},
    {"UseStandingStart",             "Cinematic Use Start",                       true},
    {"WA2HIdle",                     "Cinematic WA2 Idle",                        false},
    {"WABarrelHold",                 "Cinematic Barrel Hold",                     false},
    {"WABarrelWalk",                 "Cinematic Barrel Walk",                     false},
    {"WABoatWheelStand",             "Cinematic Boat Wheel Stand",                false},
    {"WABound01",                    "Cinematic Tied",                            false},
    {"WABound02",                    "Cinematic Tied 2",                          false},
    {"WACrankLoop",                  "Cinematic Crank Loop",                      false},
    {"WACrankStand",                 "Cinematic Crank Stand",                     false},
    {"WACrateHold",                  "Cinematic Crate Hold",                      false},
    {"WACrierStand01",               "Cinematic Crier Stand",                     false},
    {"WACrierStand02",               "Cinematic Crier Stand 2",                   false},
    {"WACrierTalk",                  "Cinematic Crier Talk",                      false},
    {"WADead01",                     "Cinematic Dead",                            false},
    {"WADead02",                     "Cinematic Dead 2",                          false},
    {"WADead04",                     "Cinematic Dead 3",                          false},
    {"WADead06",                     "Cinematic Dead 4",                          false},
    {"WADrunkDrink",                 "Cinematic Drunk Drink",                     true},
    {"WADrunkShuffleLeft",           "Cinematic Drunk Shuffle Left",              true},
    {"WADrunkShuffleRight",          "Cinematic Drunk Shuffle Right",             true},
    {"WADrunkStand",                 "Cinematic Drunk Stand",                     false},
    {"WADrunkWalk",                  "Cinematic Drunk Walk",                      false},
    {"WADrunkWalkBackwards",         "Cinematic Drunk Backwalk",                  false},
    {"WAGuardStand01",               "Cinematic Guard Stand",                     false},
    {"WAGuardStand02",               "Cinematic Guard Stand 2",                   false},
    {"WAGuardStand03",               "Cinematic Guard Stand 3",                   false},
    {"WAHammerLoop",                 "Cinematic Hammer Use",                      false},
    {"WAHang01",                     "Cinematic Hanged",                          false},
    {"WALean01",                     "Cinematic Lean",                            false},
    {"Walk",                         "Walk",                                      false},
    {"Walkbackwards",                "Cinematic Backwalk",                        false},
    {"WARowingLeft",                 "Cinematic Rowing Left",                     false},
    {"WARowingRight",                "Cinematic Rowing Right",                    false},
    {"WARowingStandLeft",            "Cinematic Rowing Stand Left",               false},
    {"WARowingStandRight",           "Cinematic Rowing Stand Right",              false},
    {"WASackHold",                   "Cinematic Sack Hold",                       false},
    {"WAScrubbing",                  "Cinematic Scrubbing",                       false},
    {"WAShovelLoop",                 "Cinematic Shovel Use",                      false},
    {"WASit01",                      "Cinematic Sit",                             false},
    {"WASit02",                      "Cinematic Sit 2",                           false},
    {"WAStandDrink",                 "Cinematic DrinkStand",                      true},
    {"WAStandEat",                   "Cinematic EatStand",                        true},
    {"WAWalk",                       "Cinematic WAWalk",                          false},
    {"WAWheelBarrowStand",           "Cinematic Barrow Stand",                    false},
    {"WAWheelBarrowWalk",            "Cinematic Barrow Walk",                     false},
    {"Whirlwind",                    "Attack Walk Stand Spin",                    false},
};
// clang-format on

} // namespace

std::optional<M2Wc3Sequence> M2Wc3SequenceFor(u16 animationId) {
    const std::string_view wow = m2::animationName(animationId);
    if (wow.empty()) {
        return std::nullopt;
    }
    for (const Row& row : kRows) {
        if (wow == row.wow) {
            return M2Wc3Sequence{row.wc3, row.nonLooping};
        }
    }
    return std::nullopt;
}

M2SequenceReport CrossM2Sequences(wem::Document& document) {
    M2SequenceReport report;
    std::vector<std::optional<M2Wc3Sequence>> named(document.clips.size());
    // The names that stay, taken before any is given, so a renamed clip can
    // never land on one of them.
    std::set<std::string> taken;
    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const wem::Clip& clip = document.clips[c];
        const wem::NativeBag::Entry* id = clip.native.find("animationId");
        if (id != nullptr && id->value >= 0 && id->value <= 0xFFFF) {
            named[c] = M2Wc3SequenceFor(static_cast<u16>(id->value));
            if (!named[c]) {
                ++report.kept;
            }
        }
        if (!named[c]) {
            taken.insert(clip.name);
        }
    }
    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        if (!named[c]) {
            continue;
        }
        wem::Clip& clip = document.clips[c];
        const std::string base(named[c]->name);
        std::string name = base;
        for (u32 n = 1; taken.count(name) != 0; ++n) {
            name = base + " - " + std::to_string(n);
        }
        taken.insert(name);
        clip.name = std::move(name);
        clip.looping = !named[c]->nonLooping;
        ++report.renamed;
    }
    if (report.renamed != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.renamed) +
                                    " animation(s) renamed to Warcraft III's vocabulary "
                                    "('Attack1H' -> 'Attack First')",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    if (report.kept != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.kept) +
                                    " animation(s) the table does not name keep World of "
                                    "Warcraft's names; the game plays none of them on its own",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout
