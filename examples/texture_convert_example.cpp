// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Texture format converter example.
///
/// Converts between BLP, BMP, DDS, JPEG, PNG, TEX and TGA texture formats.
/// The conversion direction is determined by the file extensions of the
/// input and output paths.
///
/// Usage:
///   texture_convert_example <input> [output] [options...]
///
/// Options (all apply to the output file):
///   --blp_type=<1|2>
///       BLP container version.  1 = Warcraft III era, 2 = TBC+ era (default).
///
///   --blp_compression=<true_color|paletted|jpeg|bc1|bc2|bc3>
///       Internal encoding when saving as BLP.
///         true_color  → uncompressed BGRA (BLP2 only)
///         paletted    → 256-colour palette
///         jpeg        → JPEG-compressed (raw BGRA component order)
///         bc1/bc2/bc3 → DXT block compression (BLP2 only)
///
///   --dds_format=<true_color|bc1|bc2|bc3|bc4|bc5|bc6|bc7>
///       Pixel format when saving as DDS.  The texture is converted
///       automatically via Texture::copyAsFormat().
///
///   --jpeg_quality=<1..100>
///       JPEG encoder quality level (default 75).  Only used when
///       saving as .jpg / .jpeg.
///
///   --generate_mipmaps
///       Regenerate all mip levels from the base image after loading.
///       The filter pipeline is chosen based on the texture's kind.
///
///   --texture_kind=<diffuse|normal|specular|orm|albedo|roughness|
///                   metalness|ao|gloss|emissive|other>
///       Set the semantic kind of the texture.  This affects mipmap
///       generation filters.  If omitted, the kind is guessed from
///       the file name, pixel format, or channel count.
///
/// If only one positional argument is given, the output path is derived by
/// replacing the input extension with the appropriate target format:
///   .blp → .dds,  .tex → .dds,  .dds → .blp,  .bmp → .dds,  .tga → .dds
///   .jpg → .png,  .jpeg → .png,  .png → .jpg

#include "texture_converter.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace tex = whiteout::textures;
using TFF = tex::TextureFileFormat;
using TC = tex::TextureConverter;

// ============================================================================
// Helpers
// ============================================================================

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string replace_extension(const std::string& path, const std::string& new_ext) {
    return std::filesystem::path(path).replace_extension(new_ext).string();
}

static std::string default_output_extension(TFF input_fmt) {
    switch (input_fmt) {
    case TFF::BLP:  return ".dds";
    case TFF::BMP:  return ".dds";
    case TFF::DDS:  return ".blp";
    case TFF::JPEG: return ".png";
    case TFF::PNG:  return ".jpg";
    case TFF::TEX:  return ".dds";
    case TFF::TGA:  return ".dds";
    default:        return ".dds";
    }
}

// ============================================================================
// Option parsing helpers
// ============================================================================

static std::string get_option_value(const char* arg, const char* prefix) {
    const auto prefix_len = std::strlen(prefix);
    if (std::strncmp(arg, prefix, prefix_len) == 0)
        return std::string(arg + prefix_len);
    return {};
}

static void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " <input> [output] [options...]\n"
        << "\n"
        << "Converts between BLP, BMP, DDS, JPEG, PNG, TEX and TGA texture formats.\n"
        << "If no output path is given, the extension is replaced automatically:\n"
        << "  .blp -> .dds    .bmp -> .dds    .tex -> .dds\n"
        << "  .dds -> .blp    .tga -> .dds\n"
        << "  .jpg -> .png    .png -> .jpg\n"
        << "\n"
        << "Options (all affect the output file):\n"
        << "  --blp_type=<1|2>\n"
        << "      BLP container version (default: 2)\n"
        << "\n"
        << "  --blp_compression=<true_color|paletted|jpeg|bc1|bc2|bc3>\n"
        << "      BLP internal encoding\n"
        << "\n"
        << "  --dds_format=<true_color|bc1|bc2|bc3|bc4|bc5|bc6|bc7>\n"
        << "      DDS pixel format (texture is auto-converted)\n"
        << "\n"
        << "  --jpeg_quality=<1..100>\n"
        << "      JPEG encoder quality level (default: 75)\n"
        << "\n"
        << "  --generate_mipmaps\n"
        << "      Regenerate all mip levels from the base image\n"
        << "\n"
        << "  --texture_kind=<diffuse|normal|specular|orm|albedo|roughness|\n"
        << "                  metalness|ao|gloss|emissive|other>\n"
        << "      Semantic kind of the texture (affects mipmap filters).\n"
        << "      If omitted, guessed from file name / pixel format.\n";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // -- Collect positional args and options --------------------------------
    std::vector<std::string> positional;
    std::string opt_blp_type;
    std::string opt_blp_compression;
    std::string opt_dds_format;
    std::string opt_texture_kind;
    std::string opt_jpeg_quality;
    bool opt_generate_mipmaps = false;

    for (int i = 1; i < argc; ++i) {
        std::string v;
        if ((v = get_option_value(argv[i], "--blp_type=")).size()) {
            opt_blp_type = to_lower(v);
        } else if ((v = get_option_value(argv[i], "--blp_compression=")).size()) {
            opt_blp_compression = to_lower(v);
        } else if ((v = get_option_value(argv[i], "--dds_format=")).size()) {
            opt_dds_format = to_lower(v);
        } else if ((v = get_option_value(argv[i], "--jpeg_quality=")).size()) {
            opt_jpeg_quality = v;
        } else if ((v = get_option_value(argv[i], "--texture_kind=")).size()) {
            opt_texture_kind = to_lower(v);
        } else if (std::strcmp(argv[i], "--generate_mipmaps") == 0) {
            opt_generate_mipmaps = true;
        } else if (std::strncmp(argv[i], "--", 2) == 0) {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            return 1;
        } else {
            positional.push_back(argv[i]);
        }
    }

    if (positional.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string input_path = positional[0];
    TFF input_fmt = TC::classifyPath(input_path);
    if (input_fmt == TFF::Unknown) {
        std::cerr << "Unrecognised input extension: "
                  << std::filesystem::path(input_path).extension().string() << "\n";
        return 1;
    }

    std::string output_path;
    if (positional.size() >= 2) {
        output_path = positional[1];
    } else {
        output_path = replace_extension(input_path, default_output_extension(input_fmt));
    }

    TFF output_fmt = TC::classifyPath(output_path);
    if (output_fmt == TFF::Unknown) {
        std::cerr << "Unrecognised output extension: "
                  << std::filesystem::path(output_path).extension().string() << "\n";
        return 1;
    }

    // -- Resolve BLP save options ------------------------------------------
    tex::blp::SaveOptions blp_opts;

    if (!opt_blp_type.empty()) {
        if (opt_blp_type == "1") {
            blp_opts.version = tex::blp::BlpVersion::BLP1;
        } else if (opt_blp_type == "2") {
            blp_opts.version = tex::blp::BlpVersion::BLP2;
        } else {
            std::cerr << "Invalid --blp_type value '" << opt_blp_type
                      << "'. Expected 1 or 2.\n";
            return 1;
        }
    }

    if (!opt_blp_compression.empty()) {
        if (opt_blp_compression == "true_color") {
            blp_opts.encoding = tex::blp::BlpEncoding::BGRA;
        } else if (opt_blp_compression == "paletted") {
            blp_opts.encoding = tex::blp::BlpEncoding::Palettized;
        } else if (opt_blp_compression == "jpeg") {
            blp_opts.encoding = tex::blp::BlpEncoding::JPEG;
        } else if (opt_blp_compression == "bc1" || opt_blp_compression == "bc2" ||
                   opt_blp_compression == "bc3") {
            blp_opts.encoding = tex::blp::BlpEncoding::DXT;
        } else {
            std::cerr << "Invalid --blp_compression value '" << opt_blp_compression
                      << "'. Expected true_color, paletted, jpeg, bc1, bc2, or bc3.\n";
            return 1;
        }
    }

    // -- Resolve DDS target pixel format -----------------------------------
    bool dds_convert = false;
    tex::PixelFormat dds_target_fmt = tex::PixelFormat::RGBA8;

    if (!opt_dds_format.empty()) {
        dds_convert = true;
        if (opt_dds_format == "true_color") {
            dds_target_fmt = tex::PixelFormat::RGBA8;
        } else if (opt_dds_format == "bc1") {
            dds_target_fmt = tex::PixelFormat::BC1;
        } else if (opt_dds_format == "bc2") {
            dds_target_fmt = tex::PixelFormat::BC2;
        } else if (opt_dds_format == "bc3") {
            dds_target_fmt = tex::PixelFormat::BC3;
        } else if (opt_dds_format == "bc4") {
            dds_target_fmt = tex::PixelFormat::BC4;
        } else if (opt_dds_format == "bc5") {
            dds_target_fmt = tex::PixelFormat::BC5;
        } else if (opt_dds_format == "bc6") {
            dds_target_fmt = tex::PixelFormat::BC6H;
        } else if (opt_dds_format == "bc7") {
            dds_target_fmt = tex::PixelFormat::BC7;
        } else {
            std::cerr << "Invalid --dds_format value '" << opt_dds_format
                      << "'. Expected true_color, bc1, bc2, bc3, bc4, bc5, bc6, or bc7.\n";
            return 1;
        }
    }

    std::cout << "Converting " << TC::fileFormatName(input_fmt) << " -> "
              << TC::fileFormatName(output_fmt) << "\n";

    // -- Load ---------------------------------------------------------------
    TC converter;
    auto texture = converter.load(input_path, input_fmt);
    if (!texture) {
        std::cerr << "Failed to load " << input_path;
        if (converter.hasIssues())
            std::cerr << ": " << converter.getIssues().front();
        std::cerr << "\n";
        return 1;
    }

    std::cout << "  Input:      " << input_path << "\n";
    std::cout << "  Type:       " << TC::textureTypeName(texture->type()) << "\n";
    std::cout << "  Format:     " << TC::pixelFormatName(texture->format()) << "\n";
    std::cout << "  Dimensions: " << texture->width() << "x" << texture->height() << "\n";
    std::cout << "  Mip levels: " << texture->mipCount() << "\n";

    // -- Resolve texture kind -----------------------------------------------
    if (!opt_texture_kind.empty()) {
        using K = tex::TextureKind;
        K k = K::Other;
        if      (opt_texture_kind == "diffuse")   k = K::Diffuse;
        else if (opt_texture_kind == "normal")    k = K::Normal;
        else if (opt_texture_kind == "specular")  k = K::Specular;
        else if (opt_texture_kind == "orm")       k = K::ORM;
        else if (opt_texture_kind == "albedo")    k = K::Albedo;
        else if (opt_texture_kind == "roughness") k = K::Roughness;
        else if (opt_texture_kind == "metalness") k = K::Metalness;
        else if (opt_texture_kind == "ao")        k = K::AmbientOcclusion;
        else if (opt_texture_kind == "gloss")     k = K::Gloss;
        else if (opt_texture_kind == "emissive")  k = K::Emissive;
        else if (opt_texture_kind == "other")     k = K::Other;
        else {
            std::cerr << "Invalid --texture_kind value '" << opt_texture_kind
                      << "'. Expected diffuse, normal, specular, orm, albedo, roughness,\n"
                      << "  metalness, ao, gloss, emissive, or other.\n";
            return 1;
        }
        texture->setKind(k);
        std::cout << "  Kind:       " << TC::textureKindName(k) << " (explicit)\n";
    } else {
        auto k = TC::guessTextureKind(input_path, texture->format());
        texture->setKind(k);
        std::cout << "  Kind:       " << TC::textureKindName(k) << " (guessed)\n";
    }

    // -- Generate mipmaps ---------------------------------------------------
    if (opt_generate_mipmaps) {
        bool is_bcn = texture->format() >= tex::PixelFormat::BC1 &&
                      texture->format() <= tex::PixelFormat::BC7;
        if (is_bcn) {
            std::cout << "  Decompressing " << TC::pixelFormatName(texture->format())
                      << " -> RGBA8 for mipmap generation...\n";
            *texture = texture->copyAsFormat(tex::PixelFormat::RGBA8);
        }
        std::cout << "  Generating mipmaps...\n";
        texture->generateMipmaps();
        std::cout << "  Mip levels: " << texture->mipCount() << " (regenerated)\n";
    }

    // -- Resolve JPEG quality -----------------------------------------------
    int jpeg_quality = tex::kDefaultJpegQuality;
    if (!opt_jpeg_quality.empty()) {
        int q = std::atoi(opt_jpeg_quality.c_str());
        if (q < 1 || q > 100) {
            std::cerr << "Invalid --jpeg_quality value '" << opt_jpeg_quality
                      << "'. Expected 1..100.\n";
            return 1;
        }
        jpeg_quality = q;
    }

    // Wire JPEG quality into BLP save options.
    blp_opts.jpegQuality = jpeg_quality;

    // If --jpeg_quality was given for BLP output but no explicit --blp_compression,
    // auto-select JPEG encoding so the quality option actually takes effect.
    if (output_fmt == TFF::BLP && !opt_jpeg_quality.empty() && opt_blp_compression.empty()) {
        blp_opts.encoding = tex::blp::BlpEncoding::JPEG;
    }

    // JPEG and Palettized encodings are native to BLP1.  Auto-select BLP1
    // unless the user explicitly requested a version with --blp_type.
    if (output_fmt == TFF::BLP && opt_blp_type.empty() &&
        (blp_opts.encoding == tex::blp::BlpEncoding::JPEG ||
         blp_opts.encoding == tex::blp::BlpEncoding::Palettized)) {
        blp_opts.version = tex::blp::BlpVersion::BLP1;
    }

    if (output_fmt == TFF::BLP &&
         blp_opts.encoding == tex::blp::BlpEncoding::Palettized) {
        blp_opts.dither = true;
        blp_opts.ditherStrength = 0.8f;
    }

    // -- Pre-save conversion ------------------------------------------------
    switch (output_fmt) {
    case TFF::BLP: {
        tex::PixelFormat needed = tex::PixelFormat::RGBA8;
        bool need_conversion = false;

        if (blp_opts.encoding == tex::blp::BlpEncoding::JPEG ||
            blp_opts.encoding == tex::blp::BlpEncoding::Palettized ||
            blp_opts.encoding == tex::blp::BlpEncoding::BGRA) {
            need_conversion = (texture->format() != tex::PixelFormat::RGBA8);
        } else if (opt_blp_compression == "bc1") {
            needed = tex::PixelFormat::BC1;
            need_conversion = (texture->format() != needed);
        } else if (opt_blp_compression == "bc2") {
            needed = tex::PixelFormat::BC2;
            need_conversion = (texture->format() != needed);
        } else if (opt_blp_compression == "bc3") {
            needed = tex::PixelFormat::BC3;
            need_conversion = (texture->format() != needed);
        }

        if (need_conversion) {
            std::cout << "  Converting " << TC::pixelFormatName(texture->format())
                      << " -> " << TC::pixelFormatName(needed) << " for BLP output...\n";
            *texture = texture->copyAsFormat(needed);
        }
        break;
    }
    case TFF::DDS: {
        if (dds_convert && texture->format() != dds_target_fmt) {
            std::cout << "  Converting " << TC::pixelFormatName(texture->format())
                      << " -> " << TC::pixelFormatName(dds_target_fmt) << " for DDS output...\n";
            *texture = texture->copyAsFormat(dds_target_fmt);
        }
        break;
    }
    default:
        break;
    }

    // -- Save ---------------------------------------------------------------
    bool ok = false;
    if (output_fmt == TFF::BLP)
        ok = converter.save(*texture, output_path, blp_opts);
    else if (output_fmt == TFF::JPEG)
        ok = converter.save(*texture, output_path, jpeg_quality);
    else
        ok = converter.save(*texture, output_path);

    if (!ok) {
        std::cerr << "Failed to save " << output_path;
        if (converter.hasIssues())
            std::cerr << ": " << converter.getIssues().front();
        std::cerr << "\n";
        return 1;
    }

    std::cout << "  Output:     " << output_path << "\n";
    std::cout << "  Format:     " << TC::pixelFormatName(texture->format()) << "\n";
    return 0;
}
