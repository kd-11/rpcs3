#pragma once

#include <util/types.hpp>
#include "Utilities/BitField.h"
#include "Utilities/address_range.h"

#include "Emu/RSX/VK/VulkanAPI.h"

namespace spv
{
	enum class constant_type
	{
		FLOAT = 0,
		INT,
		UINT,
		HALF,
		SHORT,
		USHORT,
		BYTE,
		UBYTE
	};

	struct vector_const_t
	{
		union
		{
			f32 f[4];
			s32 i[4];
			u32 u[4];
			u16 h[8];
			u8  b[16];
		} value;

		int m_width = 1;
		constant_type m_type = constant_type::FLOAT;

		vector_const_t() = default; // TODO

		vector_const_t(const u8 data[16])
		{
			std::memcpy(value.b, data, 16);
		}

		vector_const_t as_vf() const
		{
			vector_const_t copy = (*this);
			copy.m_type = constant_type::FLOAT;
			return copy;
		}

		vector_const_t as_vi() const
		{
			vector_const_t copy = (*this);
			copy.m_type = constant_type::INT;
			return copy;
		}

		vector_const_t as_vu() const
		{
			vector_const_t copy = (*this);
			copy.m_type = constant_type::UINT;
			return copy;
		}
	};

	struct scalar_const_t
	{
		union
		{
			f32 f;
			s32 i;
			u32 u;
			u16 h;
			u8  b;
		} value;

		constant_type m_type = constant_type::FLOAT;

		scalar_const_t() = default; // TODO

		scalar_const_t as_f() const
		{
			scalar_const_t copy = (*this);
			copy.m_type = constant_type::FLOAT;
			return copy;
		}

		scalar_const_t as_i() const
		{
			scalar_const_t copy = (*this);
			copy.m_type = constant_type::INT;
			return copy;
		}

		scalar_const_t as_u() const
		{
			scalar_const_t copy = (*this);
			copy.m_type = constant_type::UINT;
			return copy;
		}
	};

	struct vector_register_t
	{
		int vgpr_index;

		vector_register_t(s32 reg_id) : vgpr_index(reg_id)
		{}

		vector_register_t(u32 reg_id) : vgpr_index(static_cast<int>(reg_id))
		{}

		template<typename T, int A, int B>
		vector_register_t(const bf_t<T, A, B>& raw)
		{
			const T index = raw;
			vgpr_index = static_cast<int>(index);
		}
	};

	struct scalar_register_t
	{
		int sgpr_index;

		scalar_register_t(s32 reg_id) : sgpr_index(reg_id)
		{}
	};

	struct executable;

	enum class exit_code
	{
		SUCCESS = 0,
		HLT = 1,
		MFC_CMD = 2,
		RDCH3 = 3,
		STOP_AND_SIGNAL = 4
	};

	namespace constants
	{
		spv::vector_const_t make_vu(u32 x, u32 y = 0, u32 z = 0, u32 w = 0);
		spv::vector_const_t make_vi(s32 x, s32 y = 0, s32 z = 0, s32 w = 0);
		spv::vector_const_t make_vf(f32 x, f32 y = 0, f32 z = 0, f32 w = 0);
		spv::vector_const_t make_vh(u16 xl, u16 yl = 0, u16 zl = 0, u16 wl = 0, u16 xh = 0, u16 yh = 0, u16 zh = 0, u16 wh = 0);

		static inline spv::vector_const_t make_rvi(s32 w, s32 z = 0, s32 y = 0, s32 x = 0) { return make_vi(x, y, z, w); }
		static inline spv::vector_const_t make_rvu(s32 w, s32 z = 0, s32 y = 0, s32 x = 0) { return make_vu(x, y, z, w); }

		spv::scalar_const_t make_su(u32 x);
		spv::scalar_const_t make_si(s32 x);
		spv::scalar_const_t make_sf(f32 x);
		spv::scalar_const_t make_sh(u16 x);

		spv::vector_const_t spread(u32 imm);
		spv::vector_const_t spread(s32 imm);
		spv::vector_const_t spread(f32 imm);
		spv::vector_const_t spread(u16 imm);
	};

	struct instruction_t
	{
		u32 label = umax;
		u32 level = 1;
		std::string expression;

		union
		{
			u32 value = 0;
			struct
			{
				bool loop_enter : 1;
				bool loop_exit  : 1;
			};
		} flags;

		instruction_t() = default;

		instruction_t(const std::string& that, u32 level = 1)
			: expression(that), level(level)
		{}

		instruction_t(const std::string_view& that, u32 level = 1)
			: expression(that), level(level)
		{}
	};

	struct function_t
	{
		std::vector<instruction_t> instructions;
		instruction_t null_instruction = "BAD_CALL()"sv;
		u32 indent_level = 1;

		utils::address_range range() const
		{
			if (instructions.size() == 0)
			{
				return {};
			}

			const u32 head = instructions[0].label;
			u32 tail = umax;

			for (auto it = instructions.crbegin(); it != instructions.crend(); ++it)
			{
				if (it->label != umax)
				{
					tail = it->label;
					break;
				}
			}

			if (head == umax || tail == umax)
			{
				return {};
			}

			return utils::address_range::start_end(head, tail);
		}

		void indent()
		{
			indent_level++;
		}

		void outdent()
		{
			indent_level--;
		}

		void operator += (const std::string& that)
		{
			instructions.emplace_back(that, indent_level);
		}

		void operator += (const instruction_t& that)
		{
			instructions.push_back(that);
			instructions.back().level = indent_level;
		}

		void clear()
		{
			instructions.clear();
		}

		std::string compile() const
		{
			const std::array indents = {
				"",
				"  ",
				"    ",
				"      ",
				"        ",
				"          "
			};

			auto get_indent = [&indents](const instruction_t& inst) -> std::string {
				if (inst.level < indents.size())
				{
					return indents[inst.level];
				}

				std::string result = "";
				for (u32 i = 0; i < inst.level; ++i)
				{
					result += "  ";
				}
				return result;
			};

			std::stringstream os;
			for (const auto& inst : instructions)
			{
				if (!inst.expression.empty())
				{
					os << get_indent(inst) << inst.expression << "\n";
				}
			}

			return os.str();
		}

		void indent_range(u32 from, u32 to)
		{
			bool increment = false;
			for (auto& inst : instructions)
			{
				if (!increment)
				{
					if (inst.label == from)
					{
						increment = true;
					}
					continue;
				}

				inst.level++;

				if (inst.label == to)
				{
					break;
				}
			}
		}

		template <typename T> requires Integral<T>
		instruction_t& operator[](T address)
		{
			for (auto& inst : instructions)
			{
				if (inst.label == address)
				{
					return inst;
				}
			}

			return null_instruction;
		}
	};

	class assembler
	{
	public:
		// Static register allocation
		spv::vector_register_t v_tmp0 = 128;
		spv::vector_register_t v_tmp1 = 129;
		spv::vector_register_t v_tmp2 = 130;
		spv::vector_register_t v_tmp3 = 131;

		spv::scalar_register_t s_tmp0 = 0;
		spv::scalar_register_t s_tmp1 = 1;
		spv::scalar_register_t s_tmp2 = 2;
		spv::scalar_register_t s_tmp3 = 3;

		// Symbolic registers
		spv::vector_register_t v_fpscr = 1024;
		spv::scalar_register_t s_srr0 = 1025;
		spv::scalar_register_t s_flags = 1026;

		struct
		{
			spv::scalar_register_t ch_tag_mask = 1027;
			spv::scalar_register_t ch_tag_stat_count = 1028;
			spv::scalar_register_t ch_tag_stat_value = 1029;
			spv::scalar_register_t ch_tag_update = 1030;
			spv::scalar_register_t cmd_tag_id = 1031;
			spv::scalar_register_t cmd_lsa = 1032;
			spv::scalar_register_t cmd_eal = 1033;
			spv::scalar_register_t cmd_eah = 1034;
			spv::scalar_register_t cmd_size = 1035;
			spv::scalar_register_t cmd = 1036;
			spv::scalar_register_t fence = 1037;
		} mfc;

		// Debug registers
		spv::scalar_register_t s_dr0 = 1038;
		spv::scalar_register_t s_dr1 = 1039;
		spv::scalar_register_t s_dr2 = 1040;
		spv::scalar_register_t s_dr3 = 1041;

		const std::array<std::string_view, 17> m_system_register_names_s = {
			"srr0",
			"flags",
			"MFC_tag_mask",
			"MFC_tag_stat_count",
			"MFC_tag_stat_value",
			"MFC_tag_update",
			"MFC_tag_id",
			"MFC_lsa",
			"MFC_eal",
			"MFC_eah",
			"MFC_size",
			"MFC_cmd_id",
			"MFC_fence",
			"dr[0]",
			"dr[1]",
			"dr[2]",
			"dr[3]"
		};

		// Management
		void reset();
		std::unique_ptr<spv::executable> assemble(VkPipelineLayout runtime_layout);

		int get_pc() const { return m_compiler_config.end_pc; }
		bool has_dynamic_branch_target() const { return m_compiler_config.dynamic_branch_target; }
		bool has_memory_dependency() const { return m_compiler_config.uses_mfc; }

		void signal_mem() { m_compiler_config.enable_mfc(); }

		// Arithmetic ops
		void v_addsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_addui(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_adds(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_subs(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_subsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_subsi(spv::vector_register_t dst, const spv::vector_const_t& op0, spv::vector_register_t op1);
		void v_addf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_subf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_mulsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_muls(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_mulu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_mulfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_mulf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_rcpf(spv::vector_register_t dst, spv::vector_register_t op0);
		void v_rsqf(spv::vector_register_t dst, spv::vector_register_t op0);

		void s_addsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);
		void s_adds(spv::scalar_register_t dst, spv::scalar_register_t op0, spv::scalar_register_t op1);
		void s_subsi(spv::scalar_register_t dst, const spv::scalar_const_t& op0, spv::scalar_register_t op1);
		void s_subsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);

		void s_dp4si(spv::scalar_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void s_dp4s(spv::scalar_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);

		// Comparison
		void v_cmpeqsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_cmpgtsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_cmpgts(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_cmpgtu(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_cmpgtf(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_clampfi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& min, const spv::vector_const_t& max);

		// Movs
		void v_movsi(spv::vector_register_t dst, const spv::vector_const_t& src);
		void v_movs(spv::vector_register_t dst, spv::vector_register_t src);
		void v_movfi(spv::vector_register_t dst, const spv::vector_const_t& src);
		void v_storq(spv::scalar_register_t lsa, spv::vector_register_t src_reg);
		void v_storq(spv::scalar_const_t lsa, spv::vector_register_t src_reg);
		void v_loadq(spv::vector_register_t dst_reg, spv::scalar_register_t lsa);
		void v_loadq(spv::vector_register_t dst_reg, spv::scalar_const_t lsa);
		void v_sprd(spv::vector_register_t dst_reg, spv::scalar_register_t src_reg);
		void v_sprd(spv::vector_register_t dst_reg, spv::vector_register_t src_reg, int component);

		void v_storsr(spv::vector_register_t dst_reg, spv::vector_register_t src);
		void v_loadsr(spv::vector_register_t dst_reg, spv::vector_register_t src);

		void s_movsi(spv::scalar_register_t dst_reg, const spv::scalar_const_t& src);

		void s_storsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src);
		void s_loadsr(spv::scalar_register_t dst_reg, spv::scalar_register_t src);

		// Casts
		void v_fcvtu(spv::vector_register_t dst, spv::vector_register_t src);
		void v_scvtf(spv::vector_register_t dst, spv::vector_register_t src);
		void v_ucvtf(spv::vector_register_t dst, spv::vector_register_t src);

		// Bitwise
		void v_andi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t op1);
		void v_and(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_bfxi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1, const spv::scalar_const_t& op2);
		void v_bfx(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1, spv::scalar_register_t op2);
		void v_shli(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);
		void v_shri(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_xori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_ori(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1);
		void v_or(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1);

		void v_shufwi(spv::vector_register_t dst, spv::vector_register_t src, const std::array<int, 4>& shuffle);

		void s_andi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);
		void s_xori(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);
		void s_shli(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);
		void s_shri(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1);
		void s_hzor(spv::scalar_register_t dst, spv::vector_register_t op0);

		void s_xtr(spv::scalar_register_t dst, spv::vector_register_t src, int component);
		void s_ins(spv::vector_register_t dst, spv::scalar_register_t src, int component);
		void s_ins(spv::vector_register_t dst, const spv::scalar_const_t& src, spv::scalar_register_t select);

		void q_shufb(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1, spv::vector_register_t op2);
		void q_rotl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_rotr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_shr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_rotl32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_rotr32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_shl32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_shr32(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
		void q_rotl32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1);
		void q_rotr32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1);
		void q_shl32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1);
		void q_shr32i(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1);

		// Flow control
		void s_bri(const spv::scalar_const_t& target);
		void s_br(spv::scalar_register_t target);
		void s_brz(const spv::scalar_const_t& target, spv::scalar_register_t cond, spv::exit_code exit_code = spv::exit_code::SUCCESS);
		void s_brnz(const spv::scalar_const_t& target, spv::scalar_register_t cond);
		void s_heq(spv::scalar_register_t op1, spv::scalar_register_t op2);
		void s_heqi(spv::scalar_register_t op1, const spv::scalar_const_t& op2);
		void s_exit(const spv::exit_code& code);
		void s_call(const std::string_view& function, const std::vector<std::string_view>& args = {});
		bool s_loopi(const spv::scalar_const_t& target, const std::string& exit_cond);

		// General
		void unimplemented(const std::string_view& name);
		bool push_label(u32 pos);

	private:

		function_t m_block;
		std::vector<spv::vector_const_t> m_v_const_array;
		std::vector<spv::scalar_const_t> m_s_const_array;

		struct
		{
			bool dynamic_branch_target = false;
			bool uses_shufb = false;
			bool uses_qrotl32 = false;
			bool uses_qrotr32 = false;
			bool uses_qrotl = false;
			bool uses_qrotr = false;
			bool uses_qshl32 = false;
			bool uses_qshr32 = false;
			bool uses_qshl = false;
			bool uses_qshr = false;
			bool uses_mfc = false;
			int  end_pc = -1;
			int  end_pc_blob_marker = -1;

			void enable_dynamic_branch() { dynamic_branch_target = true; }
			void set_pc(int value) { end_pc = value; }

			void enable_shufb() { uses_shufb = true; }
			void enable_qrotl() { uses_qrotl = uses_qrotl32 = true; }
			void enable_qrotr() { uses_qrotr = uses_qrotr32 = true; }
			void enable_qrotl32() { uses_qrotl32 = true; }
			void enable_qrotr32() { uses_qrotr32 = true; }
			void enable_qshl() { uses_qshl = uses_qrotl = uses_qrotl32 = true; }
			void enable_qshr() { uses_qshr = uses_qrotr = uses_qrotr32 = true; }
			void enable_qshl32() { uses_qshl32 = uses_qrotl32 = true; }
			void enable_qshr32() { uses_qshr32 = uses_qrotr32 = true; }
			void enable_mfc() { uses_mfc = true; }

		} m_compiler_config;

		std::string get_const_name(const spv::vector_const_t& const_);
		std::string get_const_name(const spv::scalar_const_t& const_);

		std::string_view get_sysreg_name(const spv::scalar_register_t& reg);
		std::string_view get_sysreg_name(const spv::vector_register_t& reg);
	};
}
