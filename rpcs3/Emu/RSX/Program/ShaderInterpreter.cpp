#include "stdafx.h"
#include "ShaderInterpreter.h"

#include <unordered_set>

namespace program_common::interpreter
{
	void bitrange_foreach(u32 min, u32 max, std::function<void(u32)> func)
	{
		if (max <= min)
		{
			return;
		}

		const u32 shift = std::countr_zero(min);
		const u32 a = min >> shift;
		const u32 b = (max + max) >> shift;

		for (u32 acc = a; acc < b; acc++)
		{
			func(acc << shift);
		}
	}

	std::vector<interpreter_variant_t> get_interpreter_variants()
	{
		// Separable passes to fetch all possible variants
		std::unordered_set<u32> fs_masks;
		fs_masks.insert(0);
		bitrange_foreach(COMPILER_OPT_FS_MIN, COMPILER_OPT_FS_MAX, [&](u32 fs_opt)
		{
			fs_masks.insert(fs_opt);
		});

		// Now we add in the alpha testing variants for all fs variants.
		// Only one alpha test type is usable at once
		std::unordered_set<u32> fs_alpha_test_masks;
		for (u32 alpha_test_bit = COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
			alpha_test_bit <= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
			alpha_test_bit <<= 1)
		{
			for (const auto& mask : fs_masks)
			{
				fs_alpha_test_masks.insert(mask | alpha_test_bit);
			}
		}

		// VS
		std::unordered_set<u32> vs_masks;
		vs_masks.insert(0);
		bitrange_foreach(COMPILER_OPT_VS_MIN, COMPILER_OPT_VS_MAX, [&](u32 vs_opt)
		{
			vs_masks.insert(vs_opt);
		});

		// Merge all FS variants
		fs_masks.merge(fs_alpha_test_masks);

		// Prepare outputs
		std::vector<interpreter_variant_t> results;
		for (const auto& vs_opt : vs_masks)
		{
			for (const auto& fs_opt : fs_masks)
			{
				results.push_back({ vs_opt, fs_opt });
			}
		}

		return results;
	}
}
