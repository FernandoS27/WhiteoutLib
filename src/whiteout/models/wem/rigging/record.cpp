// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/rigging/record.h>

namespace whiteout {
namespace models {
namespace wem {

const char* ToString(RigRole role) {
    switch (role) {
    case RigRole::None:
        return "none";
    case RigRole::Body:
        return "body";
    case RigRole::Spine:
        return "spine";
    case RigRole::Neck:
        return "neck";
    case RigRole::Head:
        return "head";
    case RigRole::Clavicle:
        return "clavicle";
    case RigRole::Upper:
        return "upper";
    case RigRole::Lower:
        return "lower";
    case RigRole::Hock:
        return "hock";
    case RigRole::End:
        return "end";
    case RigRole::Toe:
        return "toe";
    case RigRole::Twist:
        return "twist";
    case RigRole::Pad:
        return "pad";
    case RigRole::Prop:
        return "prop";
    }
    return "invalid";
}

const char* ToString(RigSide side) {
    switch (side) {
    case RigSide::Centre:
        return "centre";
    case RigSide::Left:
        return "left";
    case RigSide::Right:
        return "right";
    }
    return "invalid";
}

const char* ToString(RigLimb limb) {
    switch (limb) {
    case RigLimb::Other:
        return "other";
    case RigLimb::Arm:
        return "arm";
    case RigLimb::Leg:
        return "leg";
    }
    return "invalid";
}

const char* ToString(RigPlant plant) {
    switch (plant) {
    case RigPlant::ByRole:
        return "by_role";
    case RigPlant::OnGround:
        return "on_ground";
    case RigPlant::Always:
        return "always";
    case RigPlant::Free:
        return "free";
    }
    return "invalid";
}

const char* ToString(RigSource source) {
    switch (source) {
    case RigSource::None:
        return "none";
    case RigSource::File:
        return "file";
    case RigSource::Name:
        return "name";
    case RigSource::Attachment:
        return "attachment";
    case RigSource::Shape:
        return "shape";
    case RigSource::You:
        return "you";
    }
    return "invalid";
}

} // namespace wem
} // namespace models
} // namespace whiteout
