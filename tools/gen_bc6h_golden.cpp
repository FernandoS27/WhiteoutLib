// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Regenerates tests/data/bc6h_golden_vectors.inc: BC6H blocks decoded by a
// D3D11 device, the ground truth `bc6h_test` holds the software decoder to.
//
// The blocks are ours: random bytes from a fixed seed, with the low bits of the
// first byte forced to each of the fourteen modes' headers and to the four
// reserved ones. Every block is decoded twice, on the hardware device and on
// WARP, and only a block both agree on is written; the spec defines the decode
// bit-exactly, so a disagreement is a driver bug and is reported, not recorded.
// Nothing is drawn and no window is opened.
//
// Windows only. From a Visual Studio developer prompt, in this directory:
//
//   cl /nologo /std:c++20 /EHsc /O2 gen_bc6h_golden.cpp d3d11.lib d3dcompiler.lib
//   gen_bc6h_golden.exe ..\tests\data\bc6h_golden_vectors.inc

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kPerMode = 12;

// The header of each mode as the low bits of byte 0 (BC6H's mode table).
// Modes 1 and 2 use two bits; the rest five. The last four are reserved and
// decode to zero.
struct ModeHeader {
    int bits;
    unsigned value;
};
constexpr ModeHeader kModes[] = {
    {2, 0x00}, {2, 0x01}, {5, 0x02}, {5, 0x06}, {5, 0x0A}, {5, 0x0E}, {5, 0x12}, {5, 0x16},
    {5, 0x1A}, {5, 0x1E}, {5, 0x03}, {5, 0x07}, {5, 0x0B}, {5, 0x0F}, {5, 0x13}, {5, 0x17},
    {5, 0x1B}, {5, 0x1F},
};

const char* kShader = R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float4> decoded : register(u0);
[numthreads(4, 4, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    decoded[id.xy] = source.Load(int3(id.xy, 0));
}
)";

/// Every block decoded, 16 texels of RGB as float, block-major and row-major
/// within a block. Empty on failure.
std::vector<float> Decode(D3D_DRIVER_TYPE driver, const std::vector<uint8_t>& blocks, bool isSigned) {
    const UINT count = static_cast<UINT>(blocks.size() / 16);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                 nullptr, &context)))
        return {};

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 4 * count;
    desc.Height = 4;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = isSigned ? DXGI_FORMAT_BC6H_SF16 : DXGI_FORMAT_BC6H_UF16;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{blocks.data(), static_cast<UINT>(16 * count), 0};
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID3D11ShaderResourceView> view;
    if (FAILED(device->CreateTexture2D(&desc, &init, &source)) ||
        FAILED(device->CreateShaderResourceView(source.Get(), nullptr, &view)))
        return {};

    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11UnorderedAccessView> unordered;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
        FAILED(device->CreateUnorderedAccessView(target.Get(), nullptr, &unordered)))
        return {};
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return {};

    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(kShader, std::strlen(kShader), "decode", nullptr, nullptr, "main", "cs_5_0",
                          0, 0, &code, &errors))) {
        if (errors)
            std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return {};
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                           &shader)))
        return {};

    context->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {view.Get()};
    context->CSSetShaderResources(0, 1, views);
    ID3D11UnorderedAccessView* uavs[] = {unordered.Get()};
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context->Dispatch(count, 1, 1);
    context->CopyResource(staging.Get(), target.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return {};
    std::vector<float> out(static_cast<size_t>(count) * 16 * 3);
    for (UINT b = 0; b < count; ++b)
        for (UINT t = 0; t < 16; ++t) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped.pData) +
                                                             (t / 4) * mapped.RowPitch);
            const float* texel = row + (b * 4 + t % 4) * 4;
            for (UINT c = 0; c < 3; ++c)
                out[(static_cast<size_t>(b) * 16 + t) * 3 + c] = texel[c];
        }
    context->Unmap(staging.Get(), 0);
    return out;
}

/// The half-float bits of @p v, which a BC6H decode always holds exactly.
/// Returns false for a value no half represents, which would mean the device
/// did something the format cannot.
bool HalfBits(float v, uint16_t& bits) {
    uint32_t f;
    std::memcpy(&f, &v, 4);
    const uint16_t sign = static_cast<uint16_t>((f >> 16) & 0x8000u);
    const float magnitude = std::fabs(v);
    if (magnitude == 0.0f) {
        bits = sign;
        return true;
    }
    int exponent;
    const float mantissa = std::frexp(magnitude, &exponent); // magnitude = m * 2^e, m in [0.5, 1)
    // Normal halves: value = (1024 + man) * 2^(exp - 25), exp in 1..30.
    const int halfExp = exponent + 14;
    if (halfExp >= 1 && halfExp <= 30) {
        const float scaled = std::ldexp(mantissa, 11); // in [1024, 2048)
        if (scaled != std::floor(scaled))
            return false;
        bits = static_cast<uint16_t>(sign | (halfExp << 10) | (static_cast<uint32_t>(scaled) - 1024u));
        return true;
    }
    // Subnormal halves: value = man * 2^-24.
    const float scaled = std::ldexp(magnitude, 24);
    if (scaled != std::floor(scaled) || scaled >= 1024.0f)
        return false;
    bits = static_cast<uint16_t>(sign | static_cast<uint32_t>(scaled));
    return true;
}

std::string Hex(const uint8_t* bytes, size_t count) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 15];
    }
    return out;
}

/// Writes one table; returns false if no device could decode.
bool Emit(FILE* file, const char* name, bool isSigned, std::mt19937& random) {
    std::vector<uint8_t> blocks;
    for (const ModeHeader& mode : kModes)
        for (int i = 0; i < kPerMode; ++i) {
            uint8_t block[16];
            for (uint8_t& byte : block)
                byte = static_cast<uint8_t>(random() & 0xFF);
            const unsigned mask = (1u << mode.bits) - 1u;
            block[0] = static_cast<uint8_t>((block[0] & ~mask) | mode.value);
            blocks.insert(blocks.end(), block, block + 16);
        }

    const std::vector<float> hardware = Decode(D3D_DRIVER_TYPE_HARDWARE, blocks, isSigned);
    const std::vector<float> warp = Decode(D3D_DRIVER_TYPE_WARP, blocks, isSigned);
    if (hardware.empty() || warp.empty()) {
        std::fprintf(stderr, "%s: a device could not decode (hardware %zu, WARP %zu values)\n", name,
                     hardware.size(), warp.size());
        return false;
    }

    std::fprintf(file, "inline constexpr BC6HGoldenVector %s[] = {\n", name);
    size_t written = 0;
    size_t disagreed = 0;
    for (size_t b = 0; b < blocks.size() / 16; ++b) {
        uint8_t expected[96];
        bool agree = true;
        for (size_t v = 0; v < 48 && agree; ++v) {
            const float h = hardware[b * 48 + v];
            const float w = warp[b * 48 + v];
            uint16_t bits = 0;
            if (std::memcmp(&h, &w, 4) != 0 || !HalfBits(h, bits)) {
                agree = false;
                break;
            }
            expected[v * 2] = static_cast<uint8_t>(bits >> 8);
            expected[v * 2 + 1] = static_cast<uint8_t>(bits & 0xFF);
        }
        if (!agree) {
            ++disagreed;
            continue;
        }
        std::fprintf(file, "    {\"%s\",\n     \"%s\"},\n", Hex(&blocks[b * 16], 16).c_str(),
                     Hex(expected, 96).c_str());
        ++written;
    }
    std::fprintf(file, "};\n\n");
    std::fprintf(stderr, "%s: %zu vectors, %zu blocks where the hardware and WARP disagreed\n", name,
                 written, disagreed);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gen_bc6h_golden <out.inc>\n");
        return 2;
    }
    FILE* file = std::fopen(argv[1], "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::fprintf(file,
                 "// Generated by tools/gen_bc6h_golden.cpp: BC6H blocks decoded by a D3D11\n"
                 "// hardware device and by WARP, kept where both agree. Each block is 16 bytes of\n"
                 "// hex; each expectation is 16 texels, row-major, of R, G and B as big-endian\n"
                 "// half-float bits. Regenerate rather than edit.\n\n"
                 "#pragma once\n\n"
                 "struct BC6HGoldenVector {\n"
                 "    const char* block;\n"
                 "    const char* expected;\n"
                 "};\n\n");
    std::mt19937 random(0xBC6Bu);
    const bool ok = Emit(file, "kBC6HGoldenUnsigned", false, random) &&
                    Emit(file, "kBC6HGoldenSigned", true, random);
    std::fclose(file);
    return ok ? 0 : 1;
}
