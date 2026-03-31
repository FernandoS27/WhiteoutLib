#include <whiteout/textures/texture.h>
#include <cstdio>
#include <cstring>
#include <span>

// Include internal BC7 decoder
#include "../src/whiteout/textures/bcn/bc7.h"

using namespace whiteout;
using namespace whiteout::textures;

int main() {
    // Create a 4x4 BC7 texture with one known block
    unsigned char block[16] = {0xFF, 0x00, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    auto tex = Texture::create2D(PixelFormat::BC7, 4, 4, 1);
    auto md = tex.mipData(0);
    std::memcpy(md.data(), block, 16);
    
    auto decoded = bc7::decodeTexture(tex);
    if (!decoded) {
        fprintf(stderr, "Decode failed!\n");
        return 1;
    }
    
    auto pixels = decoded->mipData(0);
    printf("Decoded 4x4 BC7 block (RGBA8):\n");
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int i = (row*4+col)*4;
            printf("  [%d,%d]: R=%3d G=%3d B=%3d A=%3d\n",
                   row, col, pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
        }
    }
    return 0;
}
