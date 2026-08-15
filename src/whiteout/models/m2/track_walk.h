
#pragma once

// ============================================================================
// forEachTrack — every per-sequence animation track in a Model, once each.
//
// The lazy sequence loader needs to reach the same set of tracks the parser
// filled, without holding pointers into a model that may since have been moved
// or copied. Walking the model by name is what makes that safe: the only
// requirement is that this list stays in step with the structures, which is one
// grep away (`AnimationTrack<` in structures/base.h).
//
// The client has the same list, in the same place: M2Init(M2Data&) descends
// into exactly these structures when it fixes up a single sequence, and skips
// everything else.
//
// ParticleAnimationTrack is deliberately absent — those keys run over a
// particle's lifetime, not over a sequence, so they are never split per
// sequence and never live in a `.anim`.
// ============================================================================

#include <whiteout/models/m2/structures.h>

namespace whiteout {
namespace m2 {

/// @brief Call @p f once per animation track in @p model.
///
/// @p f must accept both `AnimationTrack<T>&` (any T) and a bare
/// `AnimationTrackBase&` — Event::enabled and the PEDC parent event data are
/// timestamp-only tracks with no value array.
template <class F>
void forEachTrack(Model& model, F&& f) {
    for (auto& bone : model.bones) {
        f(bone.translation);
        f(bone.rotation);
        f(bone.scale);
    }
    for (auto& color : model.colors) {
        f(color.color);
        f(color.alpha);
    }
    for (auto& weight : model.textureWeights)
        f(weight.weight);
    for (auto& weight : model.parentTextureWeights)
        f(weight.weight);
    for (auto& transform : model.textureTransforms) {
        f(transform.translation);
        f(transform.rotation);
        f(transform.scaling);
    }
    for (auto& attachment : model.attachments)
        f(attachment.animate);
    for (auto& event : model.events)
        f(event.enabled);
    for (auto& track : model.parentEventData)
        f(track);
    for (auto& light : model.lights) {
        f(light.ambientColor);
        f(light.ambientIntensity);
        f(light.diffuseColor);
        f(light.diffuseIntensity);
        f(light.attenuationStart);
        f(light.attenuationEnd);
        f(light.visibility);
    }
    for (auto& camera : model.cameras) {
        f(camera.positions);
        f(camera.targetPositions);
        f(camera.roll);
        f(camera.fieldOfViewTrack);
    }
    for (auto& ribbon : model.ribbonEmitters) {
        f(ribbon.colorTrack);
        f(ribbon.alphaTrack);
        f(ribbon.heightAbove);
        f(ribbon.heightBelow);
        f(ribbon.texSlot);
        f(ribbon.visibility);
    }
    for (auto& emitter : model.particleEmitters) {
        f(emitter.emissionSpeed);
        f(emitter.speedVariation);
        f(emitter.verticalRange);
        f(emitter.horizontalRange);
        f(emitter.gravity);
        f(emitter.lifespan);
        f(emitter.emissionRate);
        f(emitter.emissionAreaWidth);
        f(emitter.emissionAreaLength);
        f(emitter.zSource);
        f(emitter.enabledIn);
    }
}

} // namespace m2
} // namespace whiteout
