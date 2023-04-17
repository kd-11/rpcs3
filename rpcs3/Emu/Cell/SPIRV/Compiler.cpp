#include "stdafx.h"
#include "Compiler.h"
#include "Runtime.h"

#include "RegisterAllocatorPass.h"

// For decode-fp16
// TODO: Make fp16 a native type
#include "Emu/RSX/rsx_utils.h"

#define SPU_DEBUG 0

namespace spv
{
	// Helpers
	namespace constants
	{
		spv::vector_const_t make_vu(u32 x, u32 y, u32 z, u32 w)
		{
			spv::vector_const_t result;
			result.m_type = spv::constant_type::UINT;
			result.m_width = 4;
			result.value.u[0] = x;
			result.value.u[1] = y;
			result.value.u[2] = z;
			result.value.u[3] = w;
			return result;
		}

		spv::vector_const_t make_vi(s32 x, s32 y, s32 z, s32 w)
		{
			spv::vector_const_t result;
			result.m_type = spv::constant_type::INT;
			result.m_width = 4;
			result.value.i[0] = x;
			result.value.i[1] = y;
			result.value.i[2] = z;
			result.value.i[3] = w;
			return result;
		}

		spv::vector_const_t make_vf(f32 x, f32 y, f32 z, f32 w)
		{
			spv::vector_const_t result;
			result.m_type = spv::constant_type::FLOAT;
			result.m_width = 4;
			result.value.f[0] = x;
			result.value.f[1] = y;
			result.value.f[2] = z;
			result.value.f[3] = w;
			return result;
		}

		spv::vector_const_t make_vh(u16 xl, u16 yl, u16 zl, u16 wl, u16 xh, u16 yh, u16 zh, u16 wh)
		{
			spv::vector_const_t result;
			result.m_type = spv::constant_type::HALF;
			result.m_width = 8;
			result.value.h[0] = xl;
			result.value.h[1] = yl;
			result.value.h[2] = zl;
			result.value.h[3] = wl;
			result.value.h[4] = xh;
			result.value.h[5] = yh;
			result.value.h[6] = zh;
			result.value.h[7] = wh;
			return result;
		}

		spv::scalar_const_t make_su(u32 x)
		{
			spv::scalar_const_t result;
			result.m_type = spv::constant_type::UINT;
			result.value.u = x;
			return result;
		}

		spv::scalar_const_t make_si(s32 x)
		{
			spv::scalar_const_t result;
			result.m_type = spv::constant_type::INT;
			result.value.i = x;
			return result;
		}

		spv::scalar_const_t make_sf(f32 x)
		{
			spv::scalar_const_t result;
			result.m_type = spv::constant_type::FLOAT;
			result.value.f = x;
			return result;
		}

		spv::scalar_const_t make_sh(u16 x)
		{
			spv::scalar_const_t result;
			result.m_type = spv::constant_type::HALF;
			result.value.u = x;
			return result;
		}

		spv::vector_const_t spread(u32 imm)
		{
			return make_vu(imm, imm, imm, imm);
		}

		spv::vector_const_t spread(s32 imm)
		{
			return make_vi(imm, imm, imm, imm);
		}

		spv::vector_const_t spread(f32 imm)
		{
			return make_vf(imm, imm, imm, imm);
		}

		spv::vector_const_t spread(u16 imm)
		{
			return make_vh(imm, imm, imm, imm, imm, imm, imm, imm);
		}

		spv::vector_const_t spread(u8 imm)
		{
			const auto v = s32(imm);
			return spread(v | v << 8 | v << 16 | v << 24);
		}
	};

	// Management
	void assembler::reset()
	{
		m_block.clear();
		m_v_const_array.clear();
		m_s_const_array.clear();
		m_compiler_config = {};
	}

	std::unique_ptr<spv::executable> assembler::assemble(VkPipelineLayout runtime_layout)
	{
		std::stringstream shaderbuf;
		// Declare common
		shaderbuf <<
			#include "SPV_Header.glsl"
			;

		if (m_compiler_config.uses_shufb)
		{
			// Sad case for shufb: GPUs support permutation instructions on both NV and AMD but glsl does not expose this.
			// TODO: Find a workaround or use SYCL
			shaderbuf <<
				#include "QSHUFB.glsl"
				;
		}

		if (m_compiler_config.uses_qrotl32)
		{
			shaderbuf <<
				#include "QROTL32.glsl"
				;
		}

		if (m_compiler_config.uses_qrotr32)
		{
			shaderbuf <<
				#include "QROTR32.glsl"
				;
		}

		if (m_compiler_config.uses_qrotl)
		{
			shaderbuf <<
				#include "QROTL.glsl"
				;
		}

		if (m_compiler_config.uses_qrotr)
		{
			shaderbuf <<
				#include "QROTR.glsl"
				;
		}

		if (m_compiler_config.uses_qshl)
		{
			shaderbuf <<
				#include "QSHL.glsl"
				;
		}

		if (m_compiler_config.uses_qshr)
		{
			shaderbuf <<
				#include "QSHR.glsl"
				;
		}

		if (m_compiler_config.uses_qshl32)
		{
			shaderbuf <<
				#include "QSHL32.glsl"
				;
		}

		if (m_compiler_config.uses_qshr32)
		{
			shaderbuf <<
				#include "QSHR32.glsl"
				;
		}

		if (m_compiler_config.uses_mfc)
		{
			// LAZY!!!!
			shaderbuf <<
				#include "MFC_write.glsl"
				;
		}

		// Constants
		for (usz idx = 0; idx < m_v_const_array.size(); ++idx)
		{
			// TODO: Non-base-type support
			const auto& const_ = m_v_const_array[idx];
			const auto [declared_type, array_size, initializer] = [](const spv::vector_const_t& const_) -> std::tuple<std::string_view, std::string_view, std::string>
			{
				switch (const_.m_type)
				{
				case spv::constant_type::FLOAT:
				{
					const auto initializer = fmt::format(
						"vec4(%.12e, %.12e, %.12e, %.12e)",
						const_.value.f[0], const_.value.f[1], const_.value.f[2], const_.value.f[3]);
					return { "vec4", "",  initializer };
				}
				case spv::constant_type::INT:
				{
					const auto initializer = fmt::format(
						"ivec4(%d, %d, %d, %d)",
						const_.value.i[0], const_.value.i[1], const_.value.i[2], const_.value.i[3]);
					return { "ivec4", "", initializer };
				}
				case spv::constant_type::UINT:
				{
					const auto initializer = fmt::format(
						"uvec4(%d, %d, %d, %d)",
						const_.value.u[0], const_.value.u[1], const_.value.u[2], const_.value.u[3]);
					return { "uvec4", "", initializer };
				}
				case spv::constant_type::HALF:
				{
					// TODO: Width optimizations
					const auto initializer = fmt::format(
						"{ vec4(%.12e, %.12e, %.12e, %.12e), vec4(%.12e, %.12e, %.12e, %.12e) };\n",
						rsx::decode_fp16(const_.value.h[0]), rsx::decode_fp16(const_.value.h[1]), rsx::decode_fp16(const_.value.h[2]), rsx::decode_fp16(const_.value.h[3]),
						rsx::decode_fp16(const_.value.h[4]), rsx::decode_fp16(const_.value.h[5]), rsx::decode_fp16(const_.value.h[6]), rsx::decode_fp16(const_.value.h[7]));
					return { "vec4", "[2]", initializer };
				}
				case spv::constant_type::SHORT:
				{
					const auto initializer = fmt::format(
						"{ ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d) };\n",
						const_.value.h[0], const_.value.h[1], const_.value.h[2], const_.value.h[3],
						const_.value.h[4], const_.value.h[5], const_.value.h[6], const_.value.h[7]);
					return { "ivec4", "[2]", initializer };
				}
				case spv::constant_type::USHORT:
				{
					const auto initializer = fmt::format(
						"{ uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u) };\n",
						const_.value.h[0], const_.value.h[1], const_.value.h[2], const_.value.h[3],
						const_.value.h[4], const_.value.h[5], const_.value.h[6], const_.value.h[7]);
					return { "uvec4", "[2]", initializer };
				}
				case spv::constant_type::BYTE:
				{
					const auto initializer = fmt::format(
						"{ ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d), ivec4(%d, %d, %d, %d) };\n",
						const_.value.b[0], const_.value.b[1], const_.value.b[2], const_.value.b[3],
						const_.value.b[4], const_.value.b[5], const_.value.b[6], const_.value.b[7],
						const_.value.b[8], const_.value.b[9], const_.value.b[10], const_.value.b[11],
						const_.value.b[12], const_.value.b[13], const_.value.b[14], const_.value.b[15]);
					return { "ivec4", "[4]", initializer };
				}
				case spv::constant_type::UBYTE:
				{
					const auto initializer = fmt::format(
						"{ uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u), uvec4(%u, %u, %u, %u) };\n",
						const_.value.b[0], const_.value.b[1], const_.value.b[2], const_.value.b[3],
						const_.value.b[4], const_.value.b[5], const_.value.b[6], const_.value.b[7],
						const_.value.b[8], const_.value.b[9], const_.value.b[10], const_.value.b[11],
						const_.value.b[12], const_.value.b[13], const_.value.b[14], const_.value.b[15]);
					return { "uvec4", "[4]", initializer };
				}
				default:
				{
					fmt::throw_exception("Unreachable");
				}
				};
			}(const_);

			shaderbuf << fmt::format(
				"const %s v_const_%d%s = %s;\n",
				declared_type, idx, array_size, initializer);
		}

		for (usz idx = 0; idx < m_s_const_array.size(); ++idx)
		{
			// TODO: Non-base-type support
			const auto& const_ = m_s_const_array[idx];
			const auto [declared_type, value] = [](const spv::scalar_const_t& const_) -> std::tuple<std::string_view, std::string>
			{
				switch (const_.m_type)
				{
				case spv::constant_type::FLOAT:
				{
					return { "float", std::to_string(const_.value.f) };
				}
				case spv::constant_type::INT:
				{
					return { "int", std::to_string(const_.value.i) };
				}
				case spv::constant_type::UINT:
				{
					return { "uint", std::to_string(const_.value.u) };
				}
				case spv::constant_type::HALF:
				{
					return { "float", std::to_string(rsx::decode_fp16(const_.value.h)) };
				}
				case spv::constant_type::SHORT:
				{
					return { "int", std::to_string(const_.value.h) };
				}
				case spv::constant_type::USHORT:
				{
					return { "uint", std::to_string(const_.value.h) };
				}
				case spv::constant_type::BYTE:
				{
					return { "int", std::to_string(const_.value.b) };
				}
				case spv::constant_type::UBYTE:
				{
					return { "uint", std::to_string(const_.value.b) };
				}
				default:
				{
					fmt::throw_exception("Unreachable");
				}
				};
			}(const_);

			shaderbuf << fmt::format(
				"const %s s_const_%d = %s;\n",
				declared_type, idx, value);
		}

		// Shadow registers
		const spv::register_allocator_output allocator_output = run_allocator_pass(m_block);
		std::vector<spv::register_initializer_t> shadow_registers;
		std::vector<spv::register_initializer_t> scratch_registers;

		for (const auto& reg_data : allocator_output.temp_regs)
		{
			if (reg_data.second.require_sync)
			{
				shadow_registers.push_back(reg_data.second);
			}
			else
			{
				scratch_registers.push_back(reg_data.second);
			}
		}

		// Lexographically sort the names
		std::sort(shadow_registers.begin(), shadow_registers.end(), FN(x.new_name < y.new_name));
		std::sort(scratch_registers.begin(), scratch_registers.end(), FN(x.new_name < y.new_name));

		// TODO: Allocator can give type hints
		for (const auto& reg : shadow_registers)
		{
			shaderbuf << "ivec4 " << reg.new_name << (reg.require_load ? " = "s + reg.old_name : ""s) << ";\n";
		}

		// Body
		shaderbuf <<
			"\n"
			"void execute()\n"
			"{\n";

		if (!scratch_registers.empty())
		{
			for (const auto& reg : scratch_registers)
			{
				shaderbuf << (reg.is_const ? "  const " : "  ") << "ivec4 " << reg.new_name << (reg.require_load ? " = "s + reg.old_name : ""s) << ";\n";
			}

			shaderbuf << "\n";
		}

		shaderbuf << m_block.compile() <<
			"}\n\n";

		// Entry
		// TODO: Retire/flush cache registers
		shaderbuf <<
			"void main()\n"
			"{\n"
			"	exit_code = 0;\n"
			"	execute();\n";

		if (!shadow_registers.empty())
		{
			shaderbuf << "\n";

			for (const auto& reg : shadow_registers)
			{
				shaderbuf << "\t" << reg.old_name << " = " << reg.new_name << ";\n";
			}
		}

#if SPU_DEBUG
		shaderbuf <<
			"\n"
			"	lr = vgpr[0].w;\n"
			"	sp = vgpr[1].w;\n"
			"	dr[11] = vgpr[95][0];\n"
			"	dr[12] = vgpr[95][1];\n"
			"	dr[13] = vgpr[95][2];\n"
			"	dr[14] = vgpr[95][3];\n"
			"	dr[15] = vgpr[9][2];\n";
#endif

		shaderbuf <<
			"}\n\n"; //qshl_mask_lookup

		const auto raw_src = shaderbuf.str();
		const auto glsl_src = fmt::replace_all(raw_src, {
			{ "//// Preprocessor", g_cfg.core.spu_accurate_xfloat ? "#define XFLOAT_PRECISE 1" : "#define XFLOAT_PRECISE 0" }
		});
		const spv::build_info exec_info = {
			.layout = runtime_layout,
			.source = glsl_src
		};

		auto result = std::make_unique<spv::executable>(exec_info);
		result->is_dynamic_branch_block = has_dynamic_branch_target();
		result->issues_memory_op = has_memory_dependency();
		result->end_pc = get_pc();
		return result;
	}

	// Arithmetic ops
	void assembler::v_addsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] + %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_adds(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] + vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_subs(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] - vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_subsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] - %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_subsi(spv::vector_register_t dst, const spv::vector_const_t& op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = %s - vgpr[%d];",
			dst.vgpr_index, get_const_name(op0), op1.vgpr_index);
	}

	void assembler::v_addf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(xfloat(vgpr[%d]) + xfloat(vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_subf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(xfloat(vgpr[%d]) - xfloat(vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_mulsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] * %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_muls(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] * vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_mulu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(uvec4(vgpr[%d]) * uvec4(vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_mulfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(xfloat(vgpr[%d]) * %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_mulf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(xfloat(vgpr[%d]) * xfloat(vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_rcpf(spv::vector_register_t dst, spv::vector_register_t op0)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(1.f / xfloat(vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index);
	}

	void assembler::v_rsqf(spv::vector_register_t dst, spv::vector_register_t op0)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(inversesqrt(xfloat(vgpr[%d])));",
			dst.vgpr_index, op0.vgpr_index);
	}

	void assembler::v_addui(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(uvec4(vgpr[%d]) + %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1.as_vu()));
	}

	void assembler::s_addsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] + %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_adds(spv::scalar_register_t dst, spv::scalar_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] + sgpr[%d];",
			dst.sgpr_index, op0.sgpr_index, op1.sgpr_index);
	}

	void assembler::s_subsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] - %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_subsi(spv::scalar_register_t dst, const spv::scalar_const_t& op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = %s - %s;",
			dst.sgpr_index, get_const_name(op0), op1.sgpr_index);
	}

	void assembler::s_dp4s(spv::scalar_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = int(dot(vgpr[%d], vgpr[%d]));",
			dst.sgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::s_dp4si(spv::scalar_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = int(dot(vgpr[%d], %s));",
			dst.sgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	// Comparison
	void assembler::v_cmpeqsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(equal(vgpr[%d], %s));",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_cmpgtsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(greaterThan(vgpr[%d], %s));",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_cmpgts(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(greaterThan(vgpr[%d], vgpr[%d]));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_cmpgtu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(greaterThan(uvec4(vgpr[%d]), uvec4(vgpr[%d])));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_cmpgtf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(greaterThan(xfloat(vgpr[%d]), xfloat(vgpr[%d])));",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_clampfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& min, const spv::vector_const_t& max)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(clamp(xfloat(vgpr[%d]), %s, %s));",
			dst.vgpr_index, op0.vgpr_index, get_const_name(min), get_const_name(max));
	}

	// Movs
	void assembler::v_movsi(spv::vector_register_t dst, const spv::vector_const_t& src)
	{
		m_block += fmt::format(
			"vgpr[%d] = %s;",
			dst.vgpr_index, get_const_name(src));
	}

	void assembler::v_movs(spv::vector_register_t dst, spv::vector_register_t src)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d];",
			dst.vgpr_index, src.vgpr_index);
	}

	void assembler::v_movfi(spv::vector_register_t dst, const spv::vector_const_t& src)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(%s);",
			dst.vgpr_index, get_const_name(src));
	}

	void assembler::s_movsi(spv::scalar_register_t dst_reg, const spv::scalar_const_t& src)
	{
		m_block += fmt::format(
			"sgpr[%d] = %s;",
			dst_reg.sgpr_index, get_const_name(src));
	}

	void assembler::s_storsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src)
	{
		m_block += fmt::format(
			"%s = sgpr[%d];",
			get_sysreg_name(dst_reg), src.sgpr_index);
	}

	void assembler::s_loadsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src)
	{
		m_block += fmt::format(
			"sgpr[%d] = %s;",
			dst_reg.sgpr_index, get_sysreg_name(src));
	}

	void assembler::v_storq(spv::scalar_register_t lsa, spv::vector_register_t src_reg)
	{
		m_block += fmt::format(
			"ls[sgpr[%d] >> 4] = _bswap(vgpr[%d]);",
			lsa.sgpr_index, src_reg.vgpr_index);
	}

	void assembler::v_storq(spv::scalar_const_t lsa, spv::vector_register_t src_reg)
	{
		m_block += fmt::format(
			"ls[%s >> 4] = _bswap(vgpr[%d]);",
			get_const_name(lsa), src_reg.vgpr_index);
	}

	void assembler::v_loadq(spv::vector_register_t dst_reg, spv::scalar_register_t lsa)
	{
		m_block += fmt::format(
			"vgpr[%d] = _bswap(ls[sgpr[%d] >> 4]);",
			dst_reg.vgpr_index, lsa.sgpr_index);
	}

	void assembler::v_loadq(spv::vector_register_t dst_reg, spv::scalar_const_t lsa)
	{
		m_block += fmt::format(
			"vgpr[%d] = _bswap(ls[%s >> 4]);",
			dst_reg.vgpr_index, get_const_name(lsa));
	}

	void assembler::v_sprd(spv::vector_register_t dst_reg, spv::scalar_register_t src_reg)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(sgpr[%d]);",
			dst_reg.vgpr_index, src_reg.sgpr_index);
	}

	void assembler::v_sprd(spv::vector_register_t dst_reg, spv::vector_register_t src_reg, int component)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(vgpr[%d][%d]);",
			dst_reg.vgpr_index, src_reg.vgpr_index, component);
	}

	// Cast
	void assembler::v_fcvtu(spv::vector_register_t dst, spv::vector_register_t src)
	{
		m_block += fmt::format(
			"vgpr[%d] = ivec4(uvec4(xfloat(vgpr[%d])));",
			dst.vgpr_index, src.vgpr_index);
	}

	void assembler::v_scvtf(spv::vector_register_t dst, spv::vector_register_t src)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(vec4(vgpr[%d]));",
			dst.vgpr_index, src.vgpr_index);
	}

	void assembler::v_ucvtf(spv::vector_register_t dst, spv::vector_register_t src)
	{
		m_block += fmt::format(
			"vgpr[%d] = floatBitsToInt(vec4(uvec4(vgpr[%d])));",
			dst.vgpr_index, src.vgpr_index);
	}

	// Bitwise
	void assembler::v_andi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] & %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_and(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] & vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_bfxi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1, const spv::scalar_const_t& op2)
	{
		m_block += fmt::format(
			"vgpr[%d] = _vbfe(vgpr[%d], %s, %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1), get_const_name(op2));
	}

	void assembler::v_bfx(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1, spv::scalar_register_t op2)
	{
		m_block += fmt::format(
			"vgpr[%d] = _vbfe(vgpr[%d], sgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index, op2.sgpr_index);
	}

	void assembler::v_shli(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] << %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] << vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_shri(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] >> %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_xori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] ^ %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_ori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] | %s;",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
	}

	void assembler::v_or(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d] | vgpr[%d];",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
	}

	void assembler::v_shufwi(spv::vector_register_t dst, spv::vector_register_t src, const std::array<int, 4>& shuffle)
	{
		const char* mask_components[4] = { "x", "y", "z", "w" };

		std::string mask = "";
		for (const auto& index : shuffle)
		{
			ensure(index < 4);
			mask += mask_components[index];
		}

		m_block += fmt::format(
			"vgpr[%d] = vgpr[%d].%s;",
			dst.vgpr_index, src.vgpr_index, mask);
	}

	void assembler::s_andi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] & %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_xori(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] ^ %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_shli(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] << %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_shri(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"sgpr[%d] = sgpr[%d] >> %s;",
			dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
	}

	void assembler::s_hzor(spv::scalar_register_t dst, spv::vector_register_t op0)
	{
		m_block += fmt::format(
			"sgpr[%d] = (vgpr[%d].x | vgpr[%d].y | vgpr[%d].z | vgpr[%d].w);",
			dst.sgpr_index, op0.vgpr_index, op0.vgpr_index, op0.vgpr_index, op0.vgpr_index);
	}

	void assembler::s_xtr(spv::scalar_register_t dst, spv::vector_register_t src, int component)
	{
		m_block += fmt::format(
			"sgpr[%d] = vgpr[%d][%d];",
			dst.sgpr_index, src.vgpr_index, component);
	}

	void assembler::s_ins(spv::vector_register_t dst, spv::scalar_register_t src, int component)
	{
		m_block += fmt::format(
			"vgpr[%d][%d] = sgpr[%d];",
			dst.vgpr_index, component, src.sgpr_index);
	}

	void assembler::s_ins(spv::vector_register_t dst, const spv::scalar_const_t& src, spv::scalar_register_t select)
	{
		m_block += fmt::format(
			"vgpr[%d][sgpr[%d]] = %s;",
			dst.vgpr_index, select.sgpr_index, get_const_name(src));
	}

	void assembler::q_shufb(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1, spv::vector_register_t op2)
	{
		m_block += fmt::format(
			"vgpr[%d] = _dqshufb(vgpr[%d], vgpr[%d], vgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.vgpr_index, op2.vgpr_index);

		m_compiler_config.enable_shufb();
	}

	// Flow control
	void assembler::s_bri(const spv::scalar_const_t& target)
	{
		if (s_loopi(target, "true"s))
		{
			return;
		}

		m_block += fmt::format(
			"pc = %s;",
			get_const_name(target.as_u()));

		m_compiler_config.set_pc(target.value.i);
	}

	void assembler::s_br(spv::scalar_register_t target)
	{
		m_block += fmt::format(
			"pc = sgpr[%d];",
			target.sgpr_index);

		m_compiler_config.enable_dynamic_branch();
	}

	void assembler::s_brz(const spv::scalar_const_t& target, spv::scalar_register_t cond, spv::exit_code exit_code)
	{
		if (exit_code == spv::exit_code::SUCCESS &&
			s_loopi(target, fmt::format("sgpr[%d] == 0", cond.sgpr_index)))
		{
			return;
		}

		m_block += fmt::format("if (sgpr[%d] == 0)", cond.sgpr_index);
		m_block += "{";
		m_block.indent();
		m_block += fmt::format("pc = %s;", get_const_name(target.as_u()));
		m_block += fmt::format("exit_code = %s;", exit_code);
		m_block += "return;";
		m_block.outdent();
		m_block += "}";

		m_compiler_config.enable_dynamic_branch();
	}

	void assembler::s_brnz(const spv::scalar_const_t& target, spv::scalar_register_t cond)
	{
		if (s_loopi(target, fmt::format("sgpr[%d] != 0", cond.sgpr_index)))
		{
			return;
		}

		m_block += fmt::format("if (sgpr[%d] != 0)", cond.sgpr_index);
		m_block += "{";
		m_block.indent();
		m_block += fmt::format("pc = %s;", get_const_name(target.as_u()));
		m_block += "return;";
		m_block.outdent();
		m_block += "}";

		m_compiler_config.enable_dynamic_branch();
	}

	void assembler::s_heq(spv::scalar_register_t op1, spv::scalar_register_t op2)
	{
		m_block += fmt::format("sgpr[%d] == sgpr[%d]", op1.sgpr_index, op2.sgpr_index);
		m_block += "{";
		m_block.indent();
		m_block += "exit_code = SPU_HLT;";
		m_block += "return;";
		m_block.outdent();
		m_block += "}";

		m_compiler_config.enable_dynamic_branch();
	}

	void assembler::s_heqi(spv::scalar_register_t op1, const spv::scalar_const_t& op2)
	{
		m_block += fmt::format("sgpr[%d] == %s", op1.sgpr_index, get_const_name(op2));
		m_block += "{";
		m_block.indent();
		m_block += "exit_code = SPU_HLT;";
		m_block += "return;";
		m_block.outdent();
		m_block += "}";

		m_compiler_config.enable_dynamic_branch();
	}

	void assembler::s_exit(const spv::exit_code& code)
	{
		m_block += fmt::format("exit_code = %s;", code);
		m_block += "return;"s;
	}

	void assembler::s_call(const std::string_view& function, const std::vector<std::string_view>& args)
	{
		std::string argsstr = "";
		for (const auto& arg : args)
		{
			if (argsstr.length() > 0)
			{
				argsstr += ", ";
			}
			argsstr += arg;
		}

		m_block += fmt::format("%s(%s);", function, argsstr);

		if (function.starts_with("MFC"))
		{
			m_compiler_config.enable_mfc();
		}
	}

	bool assembler::s_loopi(const spv::scalar_const_t& target, const std::string& exit_cond)
	{
		// internal optimization check
		const auto jmp_target = target.value.u;
		const auto range = m_block.range();

		if (jmp_target < range.start || jmp_target > range.end)
		{
			// NOP - out of range
			return false;
		}

		// Validate range
		for (u32 address = jmp_target; address < range.end; address += 4)
		{
			const auto& inst = m_block[address];
			if (inst.label != address)
			{
				// TODO - Maybe multiblock shenanigans
				return false;
			}

			if (address == jmp_target && !inst.expression.empty())
			{
				// NOP - Multiple jumps to the same address??
				return false;
			}
		}

		auto& inst = m_block[jmp_target];
		inst.expression = "do {";
		inst.flags.loop_enter = true;

		m_block.indent_range(jmp_target, range.end);

		instruction_t loop_end = "} while ("s + exit_cond + ");\n";
		loop_end.flags.loop_exit = true;
		m_block += loop_end;

		return true;
	}

	void assembler::q_rotl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotl(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qrotl();
	}

	void assembler::q_rotr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotr(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qrotr();
	}

	void assembler::q_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshl(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qshl();
	}

	void assembler::q_shr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshr(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qshr();
	}

	void assembler::q_rotl32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotl32(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qrotl32();
	}

	void assembler::q_rotr32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotr32(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qrotr32();
	}

	void assembler::q_shl32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshl32(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qshl32();
	}

	void assembler::q_shr32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshr32(vgpr[%d], sgpr[%d]);",
			dst.vgpr_index, op0.vgpr_index, op1.sgpr_index);

		m_compiler_config.enable_qshr32();
	}

	void assembler::q_rotl32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotl32(vgpr[%d], %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));

		m_compiler_config.enable_qrotl32();
	}

	void assembler::q_rotr32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qrotr32(vgpr[%d], %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));

		m_compiler_config.enable_qrotr32();
	}

	void assembler::q_shl32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshl32(vgpr[%d], %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));

		m_compiler_config.enable_qshl32();
	}

	void assembler::q_shr32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1)
	{
		m_block += fmt::format(
			"vgpr[%d] = _qshr32(vgpr[%d], %s);",
			dst.vgpr_index, op0.vgpr_index, get_const_name(op1));

		m_compiler_config.enable_qshr32();
	}

	void assembler::unimplemented(const std::string_view& name)
	{
		m_block += fmt::format("%s();", name);
	}

	bool assembler::push_label(u32 pos)
	{
		if (m_block[pos].label == pos)
		{
			return false;
		}

		instruction_t label{};
		label.label = pos;
		m_block += label;

		return true;
	}

	std::string assembler::get_const_name(const spv::vector_const_t& const_)
	{
		usz index = umax;
		for (usz i = 0; i < m_v_const_array.size(); ++i)
		{
			const auto& this_const = m_v_const_array[i];
			if (this_const.m_type == const_.m_type &&
				!std::memcmp(&this_const.value, &const_.value, sizeof(const_.value)))
			{
				index = i;
				break;
			}
		}

		if (index == umax)
		{
			index = m_v_const_array.size();
			m_v_const_array.push_back(const_);
		}

		return "v_const_" + std::to_string(index);
	}

	std::string assembler::get_const_name(const spv::scalar_const_t& const_)
	{
		usz index = umax;
		for (usz i = 0; i < m_s_const_array.size(); ++i)
		{
			const auto& this_const = m_s_const_array[i];
			if (this_const.m_type == const_.m_type &&
				!std::memcmp(&this_const.value, &const_.value, sizeof(const_.value)))
			{
				index = i;
				break;
			}
		}

		if (index == umax)
		{
			index = m_s_const_array.size();
			m_s_const_array.push_back(const_);
		}

		return "s_const_" + std::to_string(index);
	}

	std::string_view assembler::get_sysreg_name(const spv::scalar_register_t& reg)
	{
		ensure(reg.sgpr_index >= s_srr0.sgpr_index);
		const auto index = reg.sgpr_index - s_srr0.sgpr_index;
		ensure(index < m_system_register_names_s.size());

		return m_system_register_names_s[index];
	}
}
