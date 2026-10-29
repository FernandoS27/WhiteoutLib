// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file wem_convert.h
 * @brief Every WEM header plus the converting module (`whiteout_wem_convert`):
 *        the format converters, the material cores and the export passes.
 */

#include "wem.h"

#include "../cross/m2_wc3_attachments.h"
#include "../cross/m2_wc3_emitters.h"
#include "../cross/m2_wc3_lights.h"
#include "../cross/m2_wc3_sequences.h"
#include "../cross/mdx_m3_effects.h"
#include "../cross/wc3_sc2_emitters.h"
#include "converter_base.h"
#include "converters.h"
#include "d3_converter.h"
#include "fbx_converter.h"
#include "game_minimum.h"
#include "inline_models.h"
#include "interchange.h"
#include "materials/draw_order.h"
#include "materials/gltf_core.h"
#include "materials/surface_flatten.h"
#include "obj_converter.h"
#include "optimize.h"
#include "wmo_converter.h"
