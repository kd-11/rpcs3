#include "stdafx.h"
#include "ShaderInterpreter.h"

#include <unordered_set>

namespace program_common::interpreter
{
    std::vector<interpreter_variant_t> get_interpreter_variants()
    {
        // Separable passes to fetch all possible variants
        std::unordered_set<u32> fs_masks;
        for (u32 fs_opt = COMPILER_OPT_FS_MIN, fs_opt_bit = fs_opt;
            fs_opt <= COMPILER_OPT_FS_MAX; fs_opt++, fs_opt_bit <<= 1)
        {
            if (fs_opt_bit & COMPILER_OPT_ALPHA_TEST_MASK)
            {
                continue;
            }

            fs_masks.insert(fs_opt);
        }

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
        for (u32 vs_opt = COMPILER_OPT_VS_MIN; vs_opt <= COMPILER_OPT_VS_MAX; ++vs_opt)
        {
            vs_masks.insert(vs_opt);
        }

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
