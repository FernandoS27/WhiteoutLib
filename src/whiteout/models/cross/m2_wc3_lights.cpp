// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/m2_wc3_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace cross {

namespace {

/// Squares every value of a run of keys, and their tangents by the chain rule.
void SquareKeys(std::vector<u8>& bytes, u32 components, u32 valuesPerKey) {
    const std::size_t perKey = static_cast<std::size_t>(components) * valuesPerKey;
    std::vector<f32> values(bytes.size() / sizeof(f32));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(f32));
    for (std::size_t key = 0; key + perKey <= values.size(); key += perKey) {
        for (u32 c = 0; c < components; ++c) {
            const f32 value = values[key + c];
            for (u32 t = 1; t < valuesPerKey; ++t) {
                values[key + t * components + c] *= 2.0f * value;
            }
            values[key + c] = value * value;
        }
    }
    std::memcpy(bytes.data(), values.data(), values.size() * sizeof(f32));
}

/// A `.m2` light's reach is keyed, not stated: its first key in any of the
/// model's clips.
std::optional<f32> FirstKey(const wem::Document& document, u32 model, const wem::AnimChannel& channel) {
    for (const wem::Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (const wem::SubTrackContainer& container : clip.containers) {
            for (const wem::SubTrack& track : container.subTracks) {
                if (track.channel == channel.id && !track.times.empty() && track.values.size() >= sizeof(f32)) {
                    f32 value = 0.0f;
                    std::memcpy(&value, track.values.data(), sizeof(f32));
                    return value;
                }
            }
        }
    }
    if (channel.hasInitValue()) {
        f32 value = 0.0f;
        std::memcpy(&value, channel.initValue.data(), sizeof(f32));
        return value;
    }
    return std::nullopt;
}

} // namespace

std::optional<Wc3Falloff> Wc3FalloffFor(f32 start, f32 end) {
    const f64 s = std::max<f64>(start, 0.0);
    const f64 e = std::max<f64>(end, s);
    const f64 half = s + (1.0 - std::sqrt(0.5)) * (e - s);
    if (!(half > 0.0)) {
        return std::nullopt;
    }
    // With the half pinned, −ln of the falloff at the end runs down from all
    // damping (q = 0) to all quadratic (q = 1/half²).
    const f64 a = half * half;
    const f64 b = e * e;
    const f64 ln2 = std::log(2.0);
    const auto atEnd = [&](f64 q) { return (ln2 - std::log1p(q * a)) * (b / a) + std::log1p(q * b); };
    const f64 step = std::log(255.0);
    f64 q = 0.0;
    if (atEnd(0.0) > step) {
        f64 lo = 0.0;
        f64 hi = 1.0 / a;
        for (int i = 0; i < 64; ++i) {
            const f64 mid = 0.5 * (lo + hi);
            (atEnd(mid) > step ? lo : hi) = mid;
        }
        q = 0.5 * (lo + hi);
    }
    Wc3Falloff out;
    out.quadratic = static_cast<f32>(q);
    out.damping = static_cast<f32>((ln2 - std::log1p(q * a)) / a);
    return out;
}

M2LightReport CrossM2Lights(wem::Document& document) {
    M2LightReport report;
    for (u32 m = 0; m < document.models.size(); ++m) {
        wem::Model& model = document.models[m];
        for (u32 n = 0; n < model.nodes.size(); ++n) {
            wem::Node& node = model.nodes.nodes[n];
            auto* light = std::get_if<wem::LightPayload>(&node.payload);
            if (node.kind != wem::NodeKind::Light || light == nullptr || node.removed ||
                (node.native.find("wmoLightSource") == nullptr && node.native.value("m2LightType", -1) != 1)) {
                continue;
            }
            f32 start = light->attenuationStart;
            f32 end = light->attenuationEnd;
            for (wem::AnimChannel& channel : model.animChannels.channels) {
                if (channel.target.kind != wem::TrackTarget::Kind::Node || channel.target.node != n) {
                    continue;
                }
                const wem::Channel what = channel.target.channel;
                if ((what == wem::Channel::Color || what == wem::Channel::Intensity) && channel.target.sub == 0) {
                    const u32 components = what == wem::Channel::Color ? 3u : 1u;
                    SquareKeys(channel.initValue, components, 1);
                    for (wem::Clip& clip : document.clips) {
                        for (wem::SubTrackContainer& container : clip.containers) {
                            for (wem::SubTrack& track : container.subTracks) {
                                if (clip.model == m && track.channel == channel.id) {
                                    SquareKeys(track.values, components, wem::ValuesPerKey(track.interp));
                                }
                            }
                        }
                    }
                } else if (what == wem::Channel::AttenuationStart || what == wem::Channel::AttenuationEnd) {
                    if (const std::optional<f32> key = FirstKey(document, m, channel)) {
                        (what == wem::Channel::AttenuationStart ? start : end) = *key;
                    }
                }
            }
            light->color = Vector3f{light->color.x * light->color.x, light->color.y * light->color.y,
                                    light->color.z * light->color.z};
            light->intensity *= light->intensity;
            // Warcraft III has no spot: the omni light it becomes carries the
            // share of the sphere the cone lights, between its two angles.
            if (light->kind == wem::LightKind::Spot) {
                const f32 halfAngle = 0.25f * (light->hotSpot + light->falloff);
                light->intensity *= 0.5f * (1.0f - std::cos(halfAngle));
                ++report.spots;
            }
            const std::optional<Wc3Falloff> falloff = Wc3FalloffFor(start, end);
            light->quadraticFalloff = falloff ? falloff->quadratic : 0.0f;
            light->linearFalloff = falloff ? falloff->linear : 0.0f;
            // A light that reaches nothing in 12.1 falls to nothing at once.
            light->damping = falloff ? falloff->damping : 1e20f;
            report.dark += falloff ? 0u : 1u;
            ++report.restated;
        }
    }
    if (report.restated != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.restated) +
                                    " World of Warcraft point light(s) in Warcraft III's terms: colour and "
                                    "intensity squared, as 12.1 adds them, and a falloff to half where "
                                    "12.1's falls to half",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    if (report.spots != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.spots) +
                                    " spot light(s) carry their cone's share of the light into the omni "
                                    "light Warcraft III has instead",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    if (report.dark != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.dark) + " light(s) reach nothing and fall off at once",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout
