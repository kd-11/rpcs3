#pragma once
#include "../RSXTexture.h"
#include <vector>

struct MipmapLevelInfo
{
	size_t offset;
	u16 width;
	u16 height;
	u16 depth;
	u32 rowPitch;
};

/**
* Get size to store texture in a linear fashion.
* Storage is assumed to use a rowPitchAlignement boundary for every row of texture.
*/
size_t get_placed_texture_storage_size(const rsx::texture &texture, size_t rowPitchAlignement, size_t mipmapBlockAlignment=512);

/**
* Write texture data to textureData.
* Data are not packed, they are stored per rows using rowPitchAlignement.
* Similarly, offset for every mipmaplevel is aligned to rowPitchAlignement boundary.
*/
std::vector<MipmapLevelInfo> upload_placed_texture(gsl::span<gsl::byte> mapped_buffer, const rsx::texture &texture, size_t rowPitchAlignement);

/**
* Upload mipmaps individually by specifying exact offsets and pitch for target block.
*/
void upload_texture_mipmaps(gsl::span<gsl::byte> mapped_buffer, const rsx::texture &texture, std::vector<std::pair<u32, u32>> alignment_offset_info);

/**
* Get number of bytes occupied by texture in RSX mem
*/
size_t get_texture_size(const rsx::texture &texture);
