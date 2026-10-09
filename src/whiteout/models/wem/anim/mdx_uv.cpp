// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/anim/mdx_uv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace whiteout {
namespace models {
namespace wem {
namespace mdx_uv {

namespace {

constexpr f32 kTwoPi = 6.283185307179586f;
constexpr f32 kMilliseconds = 1000.0f;
/// "At rest", as a tolerance: a quarter turn's translation lands on 3e-17 and
/// would otherwise write a track that says nothing.
constexpr f32 kRest = 1e-6f;

u32 Milliseconds(f32 seconds) {
    const f32 ms = seconds * kMilliseconds;
    return ms <= 0.0f ? 0u : static_cast<u32>(ms + 0.5f);
}

/// Up to four floats of @p key, the rest zero.
void ReadFloats(geom::AttrType type, const u8* key, f32 out[4]) {
    const std::size_t count = std::min<std::size_t>(geom::AttrTypeSize(type) / sizeof(f32), 4);
    std::memcpy(out, key, count * sizeof(f32));
}

} // namespace

std::optional<UvState> StandingUv(const Matrix3x2f& matrix) {
    UvState state;
    state.angle = std::atan2(matrix.m[1][0], matrix.m[0][0]);
    const f32 c = std::cos(state.angle);
    const f32 s = std::sin(state.angle);
    state.scale = Vector2f{matrix.m[0][0] * c + matrix.m[1][0] * s,
                           matrix.m[1][1] * c - matrix.m[0][1] * s};
    if (std::fabs(matrix.m[0][1] + state.scale.y * s) > 1e-3f ||
        std::fabs(matrix.m[1][1] - state.scale.y * c) > 1e-3f) {
        return std::nullopt;
    }
    state.column = Vector2f{matrix.m[0][2], matrix.m[1][2]};
    return state;
}

Vector3f TranslationFor(const UvState& state, const Vector2f& column) {
    const f32 c = std::cos(state.angle);
    const f32 s = std::sin(state.angle);
    const f32 vx = column.x - 0.5f;
    const f32 vy = column.y - 0.5f;
    const f32 wx = c * vx + s * vy;
    const f32 wy = -s * vx + c * vy;
    return Vector3f{state.scale.x != 0.0f ? wx / state.scale.x + 0.5f : column.x,
                    state.scale.y != 0.0f ? wy / state.scale.y + 0.5f : column.y, 0.0f};
}

std::optional<UvValues> FixedValues(const Matrix3x2f& matrix) {
    const std::optional<UvState> state = StandingUv(matrix);
    if (!state.has_value()) {
        return std::nullopt;
    }
    const Vector3f translation = TranslationFor(*state, state->column);
    UvValues values;
    values.translation = Vector2f{translation.x, translation.y};
    values.angle = state->angle;
    values.scale = state->scale;
    return values;
}

Matrix3x2f MatrixOf(const UvValues& values) {
    const f32 c = std::cos(values.angle);
    const f32 s = std::sin(values.angle);
    Matrix3x2f matrix;
    matrix.m[0][0] = c * values.scale.x;
    matrix.m[0][1] = -s * values.scale.y;
    matrix.m[1][0] = s * values.scale.x;
    matrix.m[1][1] = c * values.scale.y;
    const f32 x = values.translation.x - 0.5f;
    const f32 y = values.translation.y - 0.5f;
    matrix.m[0][2] = matrix.m[0][0] * x + matrix.m[0][1] * y + 0.5f;
    matrix.m[1][2] = matrix.m[1][0] * x + matrix.m[1][1] * y + 0.5f;
    return matrix;
}

bool AtRest(Channel channel, const UvValues& values) {
    switch (channel) {
    case Channel::UvTranslate:
        return std::fabs(values.translation.x) <= kRest && std::fabs(values.translation.y) <= kRest;
    case Channel::UvRotate:
        return std::fabs(values.angle) <= kRest;
    default:
        return std::fabs(values.scale.x - 1.0f) <= kRest &&
               std::fabs(values.scale.y - 1.0f) <= kRest;
    }
}

f32 SeamlessPeriod(const Vector2f& rate) {
    f32 period = 0.0f;
    for (const f32 axis : {rate.x, rate.y}) {
        if (axis != 0.0f) {
            period = std::max(period, 1.0f / std::abs(axis));
        }
    }
    return period;
}

ScrollKeys ScrollKeysOf(const UvState& standing, const Vector2f& scrollRate) {
    ScrollKeys keys;
    const f32 period = SeamlessPeriod(scrollRate);
    keys.milliseconds = Milliseconds(period);
    if (keys.milliseconds == 0) {
        return keys;
    }
    const Vector2f travelled{std::round(scrollRate.x * period), std::round(scrollRate.y * period)};
    keys.start = TranslationFor(standing, standing.column);
    keys.end = TranslationFor(
        standing, Vector2f{standing.column.x + travelled.x, standing.column.y + travelled.y});
    return keys;
}

u32 TurnPeriod(f32 radiansPerSecond) {
    return radiansPerSecond != 0.0f ? Milliseconds(kTwoPi / std::abs(radiansPerSecond)) : 0u;
}

TurnKeys TurnKeysOf(f32 radiansPerSecond, f32 standingAngle, u32 milliseconds) {
    const f32 period = static_cast<f32>(milliseconds) / kMilliseconds;
    const f32 turns = std::max(1.0f, std::round(std::abs(radiansPerSecond) * period / kTwoPi));
    const f32 total = turns * kTwoPi * (radiansPerSecond < 0.0f ? -1.0f : 1.0f);
    TurnKeys keys;
    for (u32 step = 0; step <= 4; ++step) {
        const f32 fraction = static_cast<f32>(step) / 4.0f;
        keys.times[step] = static_cast<u32>(static_cast<f32>(milliseconds) * fraction);
        keys.angles[step] = standingAngle + total * fraction;
    }
    return keys;
}

Quaternion TurnAboutZ(f32 angle) {
    const f32 half = angle * 0.5f;
    return Quaternion{0.0f, 0.0f, std::sin(half), std::cos(half)};
}

bool IsMdxSpelling(Channel channel, geom::AttrType type) {
    return channel == Channel::UvRotate ? type == geom::AttrType::Quat
                                        : type == geom::AttrType::F32x3;
}

Vector2f DecodeKey(Channel channel, geom::AttrType type, const u8* key) {
    f32 in[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ReadFloats(type, key, in);
    if (IsMdxSpelling(channel, type)) {
        return channel == Channel::UvRotate ? Vector2f{2.0f * std::atan2(in[2], in[3]), 0.0f}
                                            : Vector2f{in[0], in[1]};
    }
    switch (channel) {
    case Channel::UvTranslate:
        return Vector2f{-in[0], -in[1]};
    case Channel::UvRotate:
        return Vector2f{in[2], 0.0f};
    default:
        return Vector2f{in[0], in[1]};
    }
}

std::vector<u8> EncodeKey(Channel channel, geom::AttrType type, const Vector2f& value) {
    f32 written[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (IsMdxSpelling(channel, type)) {
        switch (channel) {
        case Channel::UvTranslate:
            written[0] = value.x;
            written[1] = value.y;
            break;
        case Channel::UvRotate: {
            const f32 half = value.x * 0.5f;
            written[2] = std::sin(half);
            written[3] = std::cos(half);
            break;
        }
        default:
            written[0] = value.x;
            written[1] = value.y;
            written[2] = 1.0f;
            break;
        }
    } else {
        switch (channel) {
        case Channel::UvTranslate:
            written[0] = -value.x;
            written[1] = -value.y;
            break;
        case Channel::UvRotate:
            written[2] = value.x;
            break;
        default:
            written[0] = value.x;
            written[1] = value.y;
            break;
        }
    }
    std::vector<u8> out(geom::AttrTypeSize(type), 0);
    std::memcpy(out.data(), written, std::min(out.size(), sizeof(written)));
    return out;
}

Vector2f ComponentOf(Channel channel, const UvValues& values) {
    switch (channel) {
    case Channel::UvTranslate:
        return values.translation;
    case Channel::UvRotate:
        return Vector2f{values.angle, 0.0f};
    default:
        return values.scale;
    }
}

} // namespace mdx_uv
} // namespace wem
} // namespace models
} // namespace whiteout
