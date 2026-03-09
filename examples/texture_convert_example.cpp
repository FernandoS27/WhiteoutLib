// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Texture format converter example.
///
/// Converts between BLP, TEX (Diablo III SNO) and DDS texture formats.
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
/// If only one positional argument is given, the output path is derived by
/// replacing the input extension with the appropriate target format:
///   .blp → .dds,  .tex → .dds,  .dds → .blp

#include <whiteout/textures/blp/blp.h>
#include <whiteout/textures/dds/dds.h>
#include <whiteout/textures/tex/tex.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

// ============================================================================
// Helpers
// ============================================================================

enum class FileFormat { BLP, DDS, TEX, Unknown };

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string get_extension(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    return to_lower(ext);
}

static std::string replace_extension(const std::string& path, const std::string& new_ext) {
    return std::filesystem::path(path).replace_extension(new_ext).string();
}

static FileFormat classify(const std::string& path) {
    auto ext = get_extension(path);
    if (ext == ".blp") return FileFormat::BLP;
    if (ext == ".dds") return FileFormat::DDS;
    if (ext == ".tex") return FileFormat::TEX;
    return FileFormat::Unknown;
}

static const char* format_name(whiteout::textures::PixelFormat fmt) {
    switch (fmt) {
    case whiteout::textures::PixelFormat::RGBA8:   return "RGBA8";
    case whiteout::textures::PixelFormat::RGBA16F: return "RGBA16F";
    case whiteout::textures::PixelFormat::RGBA32F: return "RGBA32F";
    case whiteout::textures::PixelFormat::BC1:     return "BC1 (DXT1)";
    case whiteout::textures::PixelFormat::BC2:     return "BC2 (DXT3)";
    case whiteout::textures::PixelFormat::BC3:     return "BC3 (DXT5)";
    case whiteout::textures::PixelFormat::BC4:     return "BC4 (ATI1)";
    case whiteout::textures::PixelFormat::BC5:     return "BC5 (ATI2)";
    case whiteout::textures::PixelFormat::BC6H:    return "BC6H";
    case whiteout::textures::PixelFormat::BC7:     return "BC7";
    }
    return "Unknown";
}

static const char* type_name(whiteout::textures::TextureType type) {
    switch (type) {
    case whiteout::textures::TextureType::Texture2D:  return "2D";
    case whiteout::textures::TextureType::Texture3D:  return "3D";
    case whiteout::textures::TextureType::TextureCube: return "Cube";
    }
    return "Unknown";
}

static const char* file_format_name(FileFormat fmt) {
    switch (fmt) {
    case FileFormat::BLP: return "BLP";
    case FileFormat::DDS: return "DDS";
    case FileFormat::TEX: return "TEX";
    default:              return "Unknown";
    }
}

// ============================================================================
// BLP file helpers (class-based API requires manual file I/O)
// ============================================================================

static std::optional<whiteout::textures::Texture> load_blp_file(const std::string& path,
                                                      std::string* error) {
    whiteout::textures::blp::Parser parser;
    auto result = parser.parse(path);
    if (!result && error) {
        if (parser.hasIssues()) {
            *error = parser.getIssues().front();
        } else {
            *error = "Unknown BLP parse error";
        }
    }
    return result;
}

static bool save_blp_file(const whiteout::textures::Texture& tex, const std::string& path,
                          const whiteout::textures::blp::SaveOptions& opts, std::string* error) {
    whiteout::textures::blp::Writer writer;
    writer.write(path, tex, opts);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

// ============================================================================
// DDS file helpers (class-based API requires manual file I/O)
// ============================================================================

static std::optional<whiteout::textures::Texture> load_dds_file(const std::string& path,
                                                      std::string* error) {
    whiteout::textures::dds::Parser parser;
    auto result = parser.parse(path);
    if (parser.hasIssues()) {
        if (error) *error = parser.getIssues().front();
        return std::nullopt;
    }
    return result;
}

static bool save_dds_file(const whiteout::textures::Texture& tex, const std::string& path,
                          std::string* error) {
    whiteout::textures::dds::Writer writer;
    writer.write(path, tex);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

static std::optional<whiteout::textures::Texture> load_tex_file(const std::string& path,
                                                       std::string* error) {
    whiteout::textures::tex::Parser parser;
    auto result = parser.parse(path);
    if (parser.hasIssues()) {
        if (error) *error = parser.getIssues().front();
        return std::nullopt;
    }
    return result;
}

static bool save_tex_file(const whiteout::textures::Texture& tex, const std::string& path,
                          std::string* error) {
    whiteout::textures::tex::Writer writer;
    writer.write(path, tex);
    if (writer.hasIssues()) {
        if (error) *error = writer.getIssues().front();
        return false;
    }
    return true;
}

// ============================================================================
// Loaders
// ============================================================================

static std::optional<whiteout::textures::Texture> load_texture(const std::string& path,
                                                     FileFormat fmt,
                                                     std::string* error) {
    switch (fmt) {
    case FileFormat::BLP: return load_blp_file(path, error);
    case FileFormat::DDS: return load_dds_file(path, error);
    case FileFormat::TEX: return load_tex_file(path, error);
    default:
        if (error) *error = "unsupported input format";
        return std::nullopt;
    }
}

// ============================================================================
// Default output extension for a given input format
// ============================================================================

static std::string default_output_extension(FileFormat input_fmt) {
    switch (input_fmt) {
    case FileFormat::BLP: return ".dds";
    case FileFormat::TEX: return ".dds";
    case FileFormat::DDS: return ".blp";
    default:              return ".dds";
    }
}

// ============================================================================
// Option parsing helpers
// ============================================================================

/// Return the value after '=' in an --option=value arg, or empty string.
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
        << "Converts between BLP, TEX and DDS texture formats.\n"
        << "If no output path is given, the extension is replaced automatically:\n"
        << "  .blp -> .dds    .tex -> .dds    .dds -> .blp\n"
        << "\n"
        << "Options (all affect the output file):\n"
        << "  --blp_type=<1|2>\n"
        << "      BLP container version (default: 2)\n"
        << "\n"
        << "  --blp_compression=<true_color|paletted|jpeg|bc1|bc2|bc3>\n"
        << "      BLP internal encoding\n"
        << "\n"
        << "  --dds_format=<true_color|bc1|bc2|bc3|bc4|bc5|bc6|bc7>\n"
        << "      DDS pixel format (texture is auto-converted)\n";
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

    for (int i = 1; i < argc; ++i) {
        std::string v;
        if ((v = get_option_value(argv[i], "--blp_type=")).size()) {
            opt_blp_type = to_lower(v);
        } else if ((v = get_option_value(argv[i], "--blp_compression=")).size()) {
            opt_blp_compression = to_lower(v);
        } else if ((v = get_option_value(argv[i], "--dds_format=")).size()) {
            opt_dds_format = to_lower(v);
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
    FileFormat input_fmt = classify(input_path);
    if (input_fmt == FileFormat::Unknown) {
        std::cerr << "Unrecognised input extension: " << get_extension(input_path) << "\n";
        return 1;
    }

    std::string output_path;
    if (positional.size() >= 2) {
        output_path = positional[1];
    } else {
        output_path = replace_extension(input_path, default_output_extension(input_fmt));
    }

    FileFormat output_fmt = classify(output_path);
    if (output_fmt == FileFormat::Unknown) {
        std::cerr << "Unrecognised output extension: " << get_extension(output_path) << "\n";
        return 1;
    }

    // -- Resolve BLP save options ------------------------------------------
    whiteout::textures::blp::SaveOptions blp_opts;

    if (!opt_blp_type.empty()) {
        if (opt_blp_type == "1") {
            blp_opts.version = whiteout::textures::blp::BlpVersion::BLP1;
        } else if (opt_blp_type == "2") {
            blp_opts.version = whiteout::textures::blp::BlpVersion::BLP2;
        } else {
            std::cerr << "Invalid --blp_type value '" << opt_blp_type
                      << "'. Expected 1 or 2.\n";
            return 1;
        }
    }

    if (!opt_blp_compression.empty()) {
        if (opt_blp_compression == "true_color") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::BGRA;
        } else if (opt_blp_compression == "paletted") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::Palettized;
        } else if (opt_blp_compression == "jpeg") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::JPEG;
        } else if (opt_blp_compression == "bc1") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::DXT;
        } else if (opt_blp_compression == "bc2") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::DXT;
        } else if (opt_blp_compression == "bc3") {
            blp_opts.encoding = whiteout::textures::blp::BlpEncoding::DXT;
        } else {
            std::cerr << "Invalid --blp_compression value '" << opt_blp_compression
                      << "'. Expected true_color, paletted, jpeg, bc1, bc2, or bc3.\n";
            return 1;
        }
    }

    // -- Resolve DDS target pixel format -----------------------------------
    bool dds_convert = false;
    whiteout::textures::PixelFormat dds_target_fmt = whiteout::textures::PixelFormat::RGBA8;

    if (!opt_dds_format.empty()) {
        dds_convert = true;
        if (opt_dds_format == "true_color") {
            dds_target_fmt = whiteout::textures::PixelFormat::RGBA8;
        } else if (opt_dds_format == "bc1") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC1;
        } else if (opt_dds_format == "bc2") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC2;
        } else if (opt_dds_format == "bc3") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC3;
        } else if (opt_dds_format == "bc4") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC4;
        } else if (opt_dds_format == "bc5") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC5;
        } else if (opt_dds_format == "bc6") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC6H;
        } else if (opt_dds_format == "bc7") {
            dds_target_fmt = whiteout::textures::PixelFormat::BC7;
        } else {
            std::cerr << "Invalid --dds_format value '" << opt_dds_format
                      << "'. Expected true_color, bc1, bc2, bc3, bc4, bc5, bc6, or bc7.\n";
            return 1;
        }
    }

    std::cout << "Converting " << file_format_name(input_fmt) << " -> "
              << file_format_name(output_fmt) << "\n";

    // -- Load ---------------------------------------------------------------
    std::string error;
    auto texture = load_texture(input_path, input_fmt, &error);
    if (!texture) {
        std::cerr << "Failed to load " << input_path << ": " << error << "\n";
        return 1;
    }

    std::cout << "  Input:      " << input_path << "\n";
    std::cout << "  Type:       " << type_name(texture->type()) << "\n";
    std::cout << "  Format:     " << format_name(texture->format()) << "\n";
    std::cout << "  Dimensions: " << texture->width() << "x" << texture->height() << "\n";
    std::cout << "  Mip levels: " << texture->mipCount() << "\n";

    // -- Pre-save conversion ------------------------------------------------
    switch (output_fmt) {
    case FileFormat::BLP: {
        // Convert the texture to the format expected by the chosen BLP encoding.
        if (!opt_blp_compression.empty()) {
            whiteout::textures::PixelFormat needed = whiteout::textures::PixelFormat::RGBA8;
            if (opt_blp_compression == "bc1")      needed = whiteout::textures::PixelFormat::BC1;
            else if (opt_blp_compression == "bc2")  needed = whiteout::textures::PixelFormat::BC2;
            else if (opt_blp_compression == "bc3")  needed = whiteout::textures::PixelFormat::BC3;

            if (texture->format() != needed) {
                std::cout << "  Converting " << format_name(texture->format())
                          << " -> " << format_name(needed) << " for BLP output...\n";
                *texture = texture->copyAsFormat(needed);
            }
        }
        break;
    }
    case FileFormat::DDS: {
        if (dds_convert && texture->format() != dds_target_fmt) {
            std::cout << "  Converting " << format_name(texture->format())
                      << " -> " << format_name(dds_target_fmt) << " for DDS output...\n";
            *texture = texture->copyAsFormat(dds_target_fmt);
        }
        break;
    }
    default:
        break;
    }

    // -- Save ---------------------------------------------------------------
    bool ok = false;
    switch (output_fmt) {
    case FileFormat::BLP:
        ok = save_blp_file(*texture, output_path, blp_opts, &error);
        break;
    case FileFormat::DDS:
        ok = save_dds_file(*texture, output_path, &error);
        break;
    case FileFormat::TEX:
        ok = save_tex_file(*texture, output_path, &error);
        break;
    default:
        error = "unsupported output format";
        break;
    }

    if (!ok) {
        std::cerr << "Failed to save " << output_path << ": " << error << "\n";
        return 1;
    }

    std::cout << "  Output:     " << output_path << "\n";
    std::cout << "  Format:     " << format_name(texture->format()) << "\n";
    return 0;
}
