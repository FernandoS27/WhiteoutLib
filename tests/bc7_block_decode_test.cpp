#include <whiteout/textures/texture.h>
#include <cstdio>
#include <catch2/catch_all.hpp>
#include <cstring>
#include <span>

// Include internal BC7 decoder
#include "../src/whiteout/textures/bcn/bc7.h"

using namespace whiteout;
using namespace whiteout::textures;

TEST_CASE("BC7 block decode", "[bc7][decode]") {
    // Create a 4x4 BC7 texture with one known block
    unsigned char block[16] = {0xFF, 0x00, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    auto tex = Texture::create2D(PixelFormat::BC7, 4, 4, 1);
    auto md = tex.mipData(0);
    std::memcpy(md.data(), block, 16);
    
    auto decoded = bc7::decodeTexture(tex);
    if (!decoded) {
        FAIL("Decode failed");
    }
    
    auto pixels = decoded->mipData(0);
    REQUIRE(decoded->width() == 4);
    REQUIRE(decoded->height() == 4);
    SUCCEED("BC7 block decoded successfully");
}
