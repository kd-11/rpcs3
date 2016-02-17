#include "stdafx.h"
#include "VKHelpers.h"
#include "../GCM.h"
#include "../RSXThread.h"
#include "../RSXTexture.h"
#include "../rsx_utils.h"
#include "../Common/TextureUtils.h"

namespace vk
{
	void texture::create()
	{
	}

	u32 texture::vk_wrap_mode(u32 gcm_wrap)
	{
	}

	float texture::max_aniso(u32 gcm_aniso)
	{
		switch (gcm_aniso)
		{
		case CELL_GCM_TEXTURE_MAX_ANISO_1: return 1.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_2: return 2.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_4: return 4.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_6: return 6.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_8: return 8.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_10: return 10.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_12: return 12.0f;
		case CELL_GCM_TEXTURE_MAX_ANISO_16: return 16.0f;
		}

		LOG_ERROR(RSX, "Texture anisotropy error: bad max aniso (%d).", gcm_aniso);
		return 1.0f;
	}

	void texture::init(int index, rsx::texture& tex)
	{
	}

	void texture::remove()
	{
	}
}
