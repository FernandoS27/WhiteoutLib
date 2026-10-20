// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// The Light (.lit) and Weather (.wth) roots against the shipped files: the
/// default hero light, and the Battle.net rigs the game draws its UI heroes
/// under.
///
/// Both layouts are the PC files' own (1440 and 416 bytes), not the Switch
/// reflection's (1312 and 432), so the gate that matters is the corpus one:
/// every shipped light parses with every path in place. The `[install]` arms
/// read the game through CASC and skip with a warning when no Diablo III
/// install is found.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/d3/native/d3_native.h>

#include <cstring>
#include <vector>

TEST_CASE("D3 light: a file shorter than its root is refused", "[d3][light]") {
    namespace d3n = whiteout::sno::d3::native;
    // The 16-byte SNO header, then a root cut short: a Light needs 1440 bytes
    // and a Weather 416.
    std::vector<whiteout::u8> bytes(16 + 415, 0);
    const whiteout::u32 magic = 0xDEADBEEFu;
    std::memcpy(bytes.data(), &magic, sizeof(magic));
    CHECK_FALSE(d3n::parseWeather(bytes).has_value());
    CHECK_FALSE(d3n::parseLight(bytes).has_value());
    bytes.resize(16 + 416, 0);
    CHECK(d3n::parseWeather(bytes).has_value());
    CHECK_FALSE(d3n::parseLight(bytes).has_value());
    bytes.resize(16 + 1440, 0);
    CHECK(d3n::parseLight(bytes).has_value());
}

#if defined(WHITEOUT_HAS_CASC)

#include "d3_install.h"

#include <iostream>
#include <string>
#include <vector>

using namespace whiteout;
namespace d3n = whiteout::sno::d3::native;
using test::d3::openInstall;
using test::d3::readInstallFile;

namespace {

/// A path's value at its first node: every one of these lights holds still.
template <class Path>
f32 startOf(const Path& path) {
    return path.arNodes.empty() ? -1.0e30f : path.arNodes.front().flStart;
}

/// ColorPath bytes R,G,B over 255, times the intensity path.
Vector3f colourOf(const d3n::ColorPath& colour, const d3n::FloatPath& intensity) {
    if (colour.tHeader.nNodeCount < 1 || colour.arNodes.empty()) {
        return {-1.0f, -1.0f, -1.0f};
    }
    const u32 rgba = colour.arNodes.front().dwStartColor;
    const f32 scale = startOf(intensity) / 255.0f;
    return {static_cast<f32>(rgba & 0xFFu) * scale, static_cast<f32>((rgba >> 8) & 0xFFu) * scale,
            static_cast<f32>((rgba >> 16) & 0xFFu) * scale};
}

std::optional<d3n::Light> readLight(const storages::casc::Storage& storage,
                                    const std::string& name) {
    return d3n::parseLight(readInstallFile(storage, "Base\\Light\\" + name + ".lit"));
}

void checkColour(const Vector3f& got, f32 r, f32 g, f32 b) {
    CHECK(got.x == Catch::Approx(r).margin(1e-3));
    CHECK(got.y == Catch::Approx(g).margin(1e-3));
    CHECK(got.z == Catch::Approx(b).margin(1e-3));
}

/// The values a light rig is built from, for the record.
void print(const std::string& name, const d3n::Light& light) {
    const d3n::LightStateParams& s = light.tState0;
    const auto rgb = [](const Vector3f& c) {
        return "(" + std::to_string(c.x) + ", " + std::to_string(c.y) + ", " +
               std::to_string(c.z) + ")";
    };
    std::cout << name << ": type " << light.eLightType << ", start " << startOf(s.tStart)
              << ", end " << startOf(s.tEnd) << ", k " << startOf(s.tAttenuation0) << " / "
              << startOf(s.tAttenuation1) << " / " << startOf(s.tAttenuation2) << ", cone "
              << startOf(s.tConeAngle0) << " / " << startOf(s.tConeAngle1)
              << "\n  ambient " << rgb(colourOf(s.tAmbientColor, s.tAmbientIntensity))
              << " (intensity " << startOf(s.tAmbientIntensity) << ")\n  diffuse "
              << rgb(colourOf(s.tDiffuseColor, s.tDiffuseIntensity)) << " (intensity "
              << startOf(s.tDiffuseIntensity) << ")\n  specular "
              << rgb(colourOf(s.tSpecularColor, s.tSpecularIntensity)) << " (intensity "
              << startOf(s.tSpecularIntensity) << ")\n";
}

} // namespace

TEST_CASE("D3 light: the default hero light", "[d3][light][install]") {
    auto storage = openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    // What every Weather's hero-light slot falls back to.
    auto light = readLight(*storage, "D3PlayerLight_Actor");
    REQUIRE(light.has_value());
    print("D3PlayerLight_Actor", *light);
    CHECK(light->dwSnoId == 20099);
    CHECK(light->eLightType == static_cast<i32>(d3n::LightType::Cylindrical));
    CHECK_FALSE(light->snoTexture.valid());
    const d3n::LightStateParams& s = light->tState0;
    CHECK(startOf(s.tStart) == Catch::Approx(4.0f));
    CHECK(startOf(s.tEnd) == Catch::Approx(17.18f).margin(0.01));
    CHECK(startOf(s.tAmbientIntensity) == Catch::Approx(0.4825f).margin(1e-4));
    checkColour(colourOf(s.tAmbientColor, s.tAmbientIntensity), 0.4825f, 0.4825f, 0.4825f);
    checkColour(colourOf(s.tDiffuseColor, s.tDiffuseIntensity), 0.832f, 0.812f, 0.751f);
    checkColour(colourOf(s.tSpecularColor, s.tSpecularIntensity), 0.0f, 0.0f, 0.0f);
}

TEST_CASE("D3 light: the Battle.net rigs", "[d3][light][install]") {
    auto storage = openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    // Hero select: an all-ambient sun with a specular sheen and ~0 diffuse.
    auto sun = readLight(*storage, "BattleNet_MainScreen_Sun_Actor");
    REQUIRE(sun.has_value());
    print("BattleNet_MainScreen_Sun_Actor", *sun);
    CHECK(sun->eLightType == static_cast<i32>(d3n::LightType::Directional));
    const d3n::LightStateParams& a = sun->tState0;
    checkColour(colourOf(a.tAmbientColor, a.tAmbientIntensity), 0.216f, 0.300f, 0.406f);
    checkColour(colourOf(a.tSpecularColor, a.tSpecularIntensity), 0.224f, 0.436f, 0.546f);
    CHECK(startOf(a.tDiffuseIntensity) == Catch::Approx(0.007f).margin(1e-3));

    auto select = readLight(*storage, "BattleNet_CharacterSelect_Light");
    REQUIRE(select.has_value());
    print("BattleNet_CharacterSelect_Light", *select);
    CHECK(select->eLightType == static_cast<i32>(d3n::LightType::PointLinear));
    const d3n::LightStateParams& b = select->tState0;
    CHECK(startOf(b.tStart) == Catch::Approx(1.81f).margin(0.01));
    CHECK(startOf(b.tEnd) == Catch::Approx(11.51f).margin(0.01));
    checkColour(colourOf(b.tAmbientColor, b.tAmbientIntensity), 0.107f, 0.355f, 0.436f);

    // The friend panel's 3D hero preview, the engine's own model viewer.
    auto panel = readLight(*storage, "Battlenet_FriendInfoPanel_Light");
    REQUIRE(panel.has_value());
    print("Battlenet_FriendInfoPanel_Light", *panel);
    CHECK(panel->eLightType == static_cast<i32>(d3n::LightType::Directional));
    const d3n::LightStateParams& c = panel->tState0;
    checkColour(colourOf(c.tAmbientColor, c.tAmbientIntensity), 0.9f, 0.9f, 0.9f);
    checkColour(colourOf(c.tSpecularColor, c.tSpecularIntensity), 0.7f, 0.7f, 0.7f);
    CHECK(startOf(c.tDiffuseIntensity) == Catch::Approx(0.006f).margin(1e-3));
}

TEST_CASE("D3 light: a weather names the lights it draws under", "[d3][light][install]") {
    auto storage = openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    auto weather =
        d3n::parseWeather(readInstallFile(*storage, "Base\\Weather\\trOut_Weather.wth"));
    REQUIRE(weather.has_value());
    const d3n::WeatherStateParams& s = weather->tState0;
    CHECK(s.snoSunLightScene.id == 20385);  // trOut_Sun_noRain_Scene
    CHECK(s.snoSunLightActor.id == 20384);  // trOut_Sun_noRain_Actor
    CHECK(s.snoHeadLight.id == 20366);      // trOut_HeadLight
    CHECK(s.snoSunLightActor.group == d3n::Group::Light);

    // Hero select leaves the hero light to the engine's default.
    auto select = d3n::parseWeather(
        readInstallFile(*storage, "Base\\Weather\\BattleNet_MainScreen_Weather.wth"));
    REQUIRE(select.has_value());
    CHECK(select->tState0.snoSunLightActor.id == 148548);  // BattleNet_MainScreen_Sun_Actor
    CHECK_FALSE(select->tState0.snoPlayerLightActor.valid());
}

TEST_CASE("D3 light: every shipped light and weather parses whole", "[d3][light][install]") {
    auto storage = openInstall();
    if (!storage) {
        WARN("no Diablo III install found (set WHITEOUT_CASC_D3); skipping");
        return;
    }
    u32 lights = 0, lightsBad = 0, weathers = 0, weathersBad = 0;
    for (const std::string& path : storage->listFiles()) {
        const bool isLight = path.ends_with(".lit");
        if (!isLight && !path.ends_with(".wth")) {
            continue;
        }
        const std::vector<u8> bytes = readInstallFile(*storage, path);
        if (isLight) {
            ++lights;
            auto light = d3n::parseLight(bytes);
            // A path whose nodes did not resolve is a layout that has drifted.
            bool whole = light.has_value();
            for (const d3n::LightStateParams* s :
                 {light ? &light->tState0 : nullptr, light ? &light->tState1 : nullptr}) {
                if (!s) {
                    continue;
                }
                for (const d3n::FloatPath* p : {&s->tStart, &s->tEnd, &s->tAttenuation0,
                                                &s->tAttenuation1, &s->tAttenuation2,
                                                &s->tAmbientIntensity, &s->tDiffuseIntensity,
                                                &s->tSpecularIntensity}) {
                    whole = whole && static_cast<i32>(p->arNodes.size()) == p->tHeader.nNodeCount;
                }
                whole = whole && !s->tAmbientColor.arNodes.empty() &&
                        !s->tDiffuseColor.arNodes.empty() && !s->tSpecularColor.arNodes.empty();
            }
            lightsBad += whole ? 0 : 1;
        } else {
            ++weathers;
            weathersBad += d3n::parseWeather(bytes) ? 0 : 1;
        }
    }
    INFO(lights << " lights, " << weathers << " weathers");
    CHECK(lights > 1000);
    CHECK(lightsBad == 0);
    CHECK(weathers > 500);
    CHECK(weathersBad == 0);
}

#endif // WHITEOUT_HAS_CASC
