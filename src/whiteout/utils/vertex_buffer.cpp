// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/vertex_buffer.h"

#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>

namespace whiteout {

namespace {

size_t bytesPerComponent(AttributeType type) {
    switch (type) {
    case AttributeType::Float:
        return sizeof(f32);
    case AttributeType::Half:
        return sizeof(u16);
    case AttributeType::U32:
        return sizeof(u32);
    case AttributeType::Unorm:
    case AttributeType::Snorm:
    case AttributeType::U8:
        return sizeof(u8);
    default:
        return 0;
    }
}

void encodeFloatComponent(u8* destination, f32 value, AttributeType target_type) {
    switch (target_type) {
    case AttributeType::Float: {
        std::memcpy(destination, &value, sizeof(f32));
        break;
    }
    case AttributeType::Half: {
        f16 half_value(value);
        std::memcpy(destination, &half_value.raw, sizeof(u16));
        break;
    }
    case AttributeType::Unorm: {
        unorm8 encoded = unorm8::from_float(value);
        *destination = encoded.value;
        break;
    }
    case AttributeType::Snorm: {
        snorm8 encoded = snorm8::from_float(value);
        std::memcpy(destination, &encoded.value, sizeof(i8));
        break;
    }
    case AttributeType::U32: {
        u32 int_value = static_cast<u32>(value);
        std::memcpy(destination, &int_value, sizeof(u32));
        break;
    }
    case AttributeType::U8: {
        *destination = static_cast<u8>(value);
        break;
    }
    }
}

void encodeU32Component(u8* destination, u32 value, AttributeType target_type) {
    switch (target_type) {
    case AttributeType::U32: {
        std::memcpy(destination, &value, sizeof(u32));
        break;
    }
    case AttributeType::U8: {
        *destination = static_cast<u8>(value);
        break;
    }
    case AttributeType::Unorm: {
        // Store the raw byte value directly — u32 source values represent
        // byte-range integers (e.g. blend weights 0–255), not [0,1] floats.
        *destination = static_cast<u8>(std::min(value, u32{255}));
        break;
    }
    case AttributeType::Snorm: {
        // Store the low byte as a signed value directly.
        i8 signed_value = static_cast<i8>(value & 0xFF);
        std::memcpy(destination, &signed_value, sizeof(i8));
        break;
    }
    default: {
        encodeFloatComponent(destination, static_cast<f32>(value), target_type);
        break;
    }
    }
}

} // anonymous namespace

struct VertexBufferBuilder::Impl {
    union ComponentValue {
        f32 as_f32;
        u32 as_u32;

        ComponentValue() {}
        ~ComponentValue() {}
    };

    struct PendingAttribute {
        std::vector<ComponentValue> source_data;
        AttributeClass attr_class;
        AttributeType attr_type;
        size_t component_count;
        bool is_u32_source = false;
        std::function<void(u8*, ComponentValue, AttributeType)> encoder;

        size_t vertexCount() const {
            return source_data.size() / component_count;
        }

        void encodeInto(u8* destination, size_t vertex_index) const {
            const size_t component_size = bytesPerComponent(attr_type);

            for (size_t component = 0; component < component_count; ++component) {
                const size_t source_index = vertex_index * component_count + component;
                u8* component_destination = destination + component * component_size;

                if (is_u32_source) {
                    encodeU32Component(component_destination, source_data[source_index].as_u32,
                                       attr_type);
                } else {
                    encodeFloatComponent(component_destination, source_data[source_index].as_f32,
                                         attr_type);
                }
            }
        }
    };

    std::vector<PendingAttribute> attributes;

    template <typename SourceType>
    void declareAttributeImpl(std::span<const SourceType> src_data, AttributeClass attr_class,
                              AttributeType attr_type, size_t components) {
        PendingAttribute attribute;
        attribute.source_data.resize(src_data.size());
        std::memcpy(attribute.source_data.data(), src_data.data(),
                    src_data.size() * sizeof(SourceType));
        attribute.attr_class = attr_class;
        attribute.attr_type = attr_type;
        attribute.component_count = components;
        attribute.is_u32_source = std::is_same_v<SourceType, u32>;
        attributes.push_back(std::move(attribute));
    }
};

VertexBufferBuilder::VertexBufferBuilder() : impl_(std::make_unique<Impl>()) {}

VertexBufferBuilder::~VertexBufferBuilder() = default;

VertexBufferBuilder::VertexBufferBuilder(VertexBufferBuilder&&) noexcept = default;

VertexBufferBuilder& VertexBufferBuilder::operator=(VertexBufferBuilder&&) noexcept = default;

VertexBufferBuilder& VertexBufferBuilder::declareAttribute(std::span<const f32> src_data,
                                                           AttributeClass attr_class,
                                                           AttributeType attr_type,
                                                           size_t components) {
    impl_->declareAttributeImpl(src_data, attr_class, attr_type, components);
    return *this;
}

VertexBufferBuilder& VertexBufferBuilder::declareAttribute(std::span<const u32> src_data,
                                                           AttributeClass attr_class,
                                                           AttributeType attr_type,
                                                           size_t components) {
    impl_->declareAttributeImpl(src_data, attr_class, attr_type, components);
    return *this;
}

VertexBuffer VertexBufferBuilder::build() const {
    const auto& attributes = impl_->attributes;

    if (attributes.empty()) {
        return {};
    }

    const size_t vertex_count = attributes[0].vertexCount();

    VertexBuffer result;

    // Compute layout and stride
    size_t current_offset = 0;
    for (const auto& attribute : attributes) {
        const size_t component_size = bytesPerComponent(attribute.attr_type);
        VertexBuffer::Attribute layout_entry;
        layout_entry.attr_class = attribute.attr_class;
        layout_entry.attr_type = attribute.attr_type;
        layout_entry.component_count = attribute.component_count;
        layout_entry.offset = current_offset;
        result.layout.push_back(layout_entry);
        current_offset += component_size * attribute.component_count;
    }
    result.vertex_stride = static_cast<u32>(current_offset);

    // Allocate and fill interleaved data
    result.data.resize(vertex_count * result.vertex_stride, 0);

    for (size_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
        u8* vertex_base = result.data.data() + vertex_index * result.vertex_stride;

        for (size_t attr_index = 0; attr_index < attributes.size(); ++attr_index) {
            u8* attribute_destination = vertex_base + result.layout[attr_index].offset;
            attributes[attr_index].encodeInto(attribute_destination, vertex_index);
        }
    }

    return result;
}

} // namespace whiteout
