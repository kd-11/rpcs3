#pragma once

#include "SPURecompiler.h"

#include <functional>

// TODO:
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

	struct SPUSPV_block;

	enum class exit_code
	{
		SUCCESS = 0,
		HLT = 1,
		MFC_CMD = 2,
		RDCH3 = 3
	};
}

namespace spv_constant
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

class spv_emitter
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
	spv::scalar_register_t s_srr0  = 1025;
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

	const std::array<std::string_view, 13> m_system_register_names_s = {
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
		"MFC_fence"
	};

	// Management
	void reset();
	std::unique_ptr<spv::SPUSPV_block> compile();

	int get_pc() const { return m_compiler_config.end_pc; }
	bool has_dynamic_branch_target() const { return m_compiler_config.dynamic_branch_target; }
	bool has_memory_dependency() const { return m_compiler_config.uses_mfc; }

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
	void s_hzor(spv::scalar_register_t dst, spv::vector_register_t op0);

	void s_xtr(spv::scalar_register_t dst, spv::vector_register_t src, int component);
	void s_ins(spv::vector_register_t dst, spv::scalar_register_t src, int component);
	void s_ins(spv::vector_register_t dst, const spv::scalar_const_t& src, spv::scalar_register_t select);

	void q_shufb(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1, spv::vector_register_t op2);
	void q_rotl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
	void q_rotr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
	void q_shl(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);
	void q_shr(spv::vector_register_t dst, spv::vector_register_t op0, spv::scalar_register_t op1);

	// Flow control
	void s_bri(const spv::scalar_const_t& target);
	void s_br(spv::scalar_register_t target);
	void s_brz(const spv::scalar_const_t& target, spv::scalar_register_t cond, spv::exit_code exit_code = spv::exit_code::SUCCESS);
	void s_brnz(const spv::scalar_const_t& target, spv::scalar_register_t cond);
	void s_heq(spv::scalar_register_t op1, spv::scalar_register_t op2);
	void s_heqi(spv::scalar_register_t op1, const spv::scalar_const_t& op2);
	void s_exit(const spv::exit_code& code);
	void s_call(const std::string_view& function, const std::vector<std::string_view>& args = {});

	// General
	void unimplemented(const std::string_view& name);

private:
	std::string m_block;
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

		void enable_dynamic_branch() { dynamic_branch_target = true; }
		void set_pc(int value) { end_pc = value; }

		void enable_shufb() { uses_shufb = true; }
		void enable_qrotl() { uses_qrotl = uses_qrotl32 = true; }
		void enable_qrotr() { uses_qrotr = uses_qrotr32 = true; }
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

// SPU SPIR-V Recompiler
class spv_recompiler : public spu_recompiler_base
{
public:
	spv_recompiler();

	virtual void init() override;

	virtual spu_function_t compile(spu_program&&) override;

private:
	spv_emitter c;

public:
	void UNK(spu_opcode_t op);

	void STOP(spu_opcode_t op);
	void LNOP(spu_opcode_t op);
	void SYNC(spu_opcode_t op);
	void DSYNC(spu_opcode_t op);
	void MFSPR(spu_opcode_t op);
	void RDCH(spu_opcode_t op);
	void RCHCNT(spu_opcode_t op);
	void SF(spu_opcode_t op);
	void OR(spu_opcode_t op);
	void BG(spu_opcode_t op);
	void SFH(spu_opcode_t op);
	void NOR(spu_opcode_t op);
	void ABSDB(spu_opcode_t op);
	void ROT(spu_opcode_t op);
	void ROTM(spu_opcode_t op);
	void ROTMA(spu_opcode_t op);
	void SHL(spu_opcode_t op);
	void ROTH(spu_opcode_t op);
	void ROTHM(spu_opcode_t op);
	void ROTMAH(spu_opcode_t op);
	void SHLH(spu_opcode_t op);
	void ROTI(spu_opcode_t op);
	void ROTMI(spu_opcode_t op);
	void ROTMAI(spu_opcode_t op);
	void SHLI(spu_opcode_t op);
	void ROTHI(spu_opcode_t op);
	void ROTHMI(spu_opcode_t op);
	void ROTMAHI(spu_opcode_t op);
	void SHLHI(spu_opcode_t op);
	void A(spu_opcode_t op);
	void AND(spu_opcode_t op);
	void CG(spu_opcode_t op);
	void AH(spu_opcode_t op);
	void NAND(spu_opcode_t op);
	void AVGB(spu_opcode_t op);
	void MTSPR(spu_opcode_t op);
	void WRCH(spu_opcode_t op);
	void BIZ(spu_opcode_t op);
	void BINZ(spu_opcode_t op);
	void BIHZ(spu_opcode_t op);
	void BIHNZ(spu_opcode_t op);
	void STOPD(spu_opcode_t op);
	void STQX(spu_opcode_t op);
	void BI(spu_opcode_t op);
	void BISL(spu_opcode_t op);
	void IRET(spu_opcode_t op);
	void BISLED(spu_opcode_t op);
	void HBR(spu_opcode_t op);
	void GB(spu_opcode_t op);
	void GBH(spu_opcode_t op);
	void GBB(spu_opcode_t op);
	void FSM(spu_opcode_t op);
	void FSMH(spu_opcode_t op);
	void FSMB(spu_opcode_t op);
	void FREST(spu_opcode_t op);
	void FRSQEST(spu_opcode_t op);
	void LQX(spu_opcode_t op);
	void ROTQBYBI(spu_opcode_t op);
	void ROTQMBYBI(spu_opcode_t op);
	void SHLQBYBI(spu_opcode_t op);
	void CBX(spu_opcode_t op);
	void CHX(spu_opcode_t op);
	void CWX(spu_opcode_t op);
	void CDX(spu_opcode_t op);
	void ROTQBI(spu_opcode_t op);
	void ROTQMBI(spu_opcode_t op);
	void SHLQBI(spu_opcode_t op);
	void ROTQBY(spu_opcode_t op);
	void ROTQMBY(spu_opcode_t op);
	void SHLQBY(spu_opcode_t op);
	void ORX(spu_opcode_t op);
	void CBD(spu_opcode_t op);
	void CHD(spu_opcode_t op);
	void CWD(spu_opcode_t op);
	void CDD(spu_opcode_t op);
	void ROTQBII(spu_opcode_t op);
	void ROTQMBII(spu_opcode_t op);
	void SHLQBII(spu_opcode_t op);
	void ROTQBYI(spu_opcode_t op);
	void ROTQMBYI(spu_opcode_t op);
	void SHLQBYI(spu_opcode_t op);
	void NOP(spu_opcode_t op);
	void CGT(spu_opcode_t op);
	void XOR(spu_opcode_t op);
	void CGTH(spu_opcode_t op);
	void EQV(spu_opcode_t op);
	void CGTB(spu_opcode_t op);
	void SUMB(spu_opcode_t op);
	void HGT(spu_opcode_t op);
	void CLZ(spu_opcode_t op);
	void XSWD(spu_opcode_t op);
	void XSHW(spu_opcode_t op);
	void CNTB(spu_opcode_t op);
	void XSBH(spu_opcode_t op);
	void CLGT(spu_opcode_t op);
	void ANDC(spu_opcode_t op);
	void FCGT(spu_opcode_t op);
	void DFCGT(spu_opcode_t op);
	void FA(spu_opcode_t op);
	void FS(spu_opcode_t op);
	void FM(spu_opcode_t op);
	void CLGTH(spu_opcode_t op);
	void ORC(spu_opcode_t op);
	void FCMGT(spu_opcode_t op);
	void DFCMGT(spu_opcode_t op);
	void DFA(spu_opcode_t op);
	void DFS(spu_opcode_t op);
	void DFM(spu_opcode_t op);
	void CLGTB(spu_opcode_t op);
	void HLGT(spu_opcode_t op);
	void DFMA(spu_opcode_t op);
	void DFMS(spu_opcode_t op);
	void DFNMS(spu_opcode_t op);
	void DFNMA(spu_opcode_t op);
	void CEQ(spu_opcode_t op);
	void MPYHHU(spu_opcode_t op);
	void ADDX(spu_opcode_t op);
	void SFX(spu_opcode_t op);
	void CGX(spu_opcode_t op);
	void BGX(spu_opcode_t op);
	void MPYHHA(spu_opcode_t op);
	void MPYHHAU(spu_opcode_t op);
	void FSCRRD(spu_opcode_t op);
	void FESD(spu_opcode_t op);
	void FRDS(spu_opcode_t op);
	void FSCRWR(spu_opcode_t op);
	void DFTSV(spu_opcode_t op);
	void FCEQ(spu_opcode_t op);
	void DFCEQ(spu_opcode_t op);
	void MPY(spu_opcode_t op);
	void MPYH(spu_opcode_t op);
	void MPYHH(spu_opcode_t op);
	void MPYS(spu_opcode_t op);
	void CEQH(spu_opcode_t op);
	void FCMEQ(spu_opcode_t op);
	void DFCMEQ(spu_opcode_t op);
	void MPYU(spu_opcode_t op);
	void CEQB(spu_opcode_t op);
	void FI(spu_opcode_t op);
	void HEQ(spu_opcode_t op);
	void CFLTS(spu_opcode_t op);
	void CFLTU(spu_opcode_t op);
	void CSFLT(spu_opcode_t op);
	void CUFLT(spu_opcode_t op);
	void BRZ(spu_opcode_t op);
	void STQA(spu_opcode_t op);
	void BRNZ(spu_opcode_t op);
	void BRHZ(spu_opcode_t op);
	void BRHNZ(spu_opcode_t op);
	void STQR(spu_opcode_t op);
	void BRA(spu_opcode_t op);
	void LQA(spu_opcode_t op);
	void BRASL(spu_opcode_t op);
	void BR(spu_opcode_t op);
	void FSMBI(spu_opcode_t op);
	void BRSL(spu_opcode_t op);
	void LQR(spu_opcode_t op);
	void IL(spu_opcode_t op);
	void ILHU(spu_opcode_t op);
	void ILH(spu_opcode_t op);
	void IOHL(spu_opcode_t op);
	void ORI(spu_opcode_t op);
	void ORHI(spu_opcode_t op);
	void ORBI(spu_opcode_t op);
	void SFI(spu_opcode_t op);
	void SFHI(spu_opcode_t op);
	void ANDI(spu_opcode_t op);
	void ANDHI(spu_opcode_t op);
	void ANDBI(spu_opcode_t op);
	void AI(spu_opcode_t op);
	void AHI(spu_opcode_t op);
	void STQD(spu_opcode_t op);
	void LQD(spu_opcode_t op);
	void XORI(spu_opcode_t op);
	void XORHI(spu_opcode_t op);
	void XORBI(spu_opcode_t op);
	void CGTI(spu_opcode_t op);
	void CGTHI(spu_opcode_t op);
	void CGTBI(spu_opcode_t op);
	void HGTI(spu_opcode_t op);
	void CLGTI(spu_opcode_t op);
	void CLGTHI(spu_opcode_t op);
	void CLGTBI(spu_opcode_t op);
	void HLGTI(spu_opcode_t op);
	void MPYI(spu_opcode_t op);
	void MPYUI(spu_opcode_t op);
	void CEQI(spu_opcode_t op);
	void CEQHI(spu_opcode_t op);
	void CEQBI(spu_opcode_t op);
	void HEQI(spu_opcode_t op);
	void HBRA(spu_opcode_t op);
	void HBRR(spu_opcode_t op);
	void ILA(spu_opcode_t op);
	void SELB(spu_opcode_t op);
	void SHUFB(spu_opcode_t op);
	void MPYA(spu_opcode_t op);
	void FNMS(spu_opcode_t op);
	void FMA(spu_opcode_t op);
	void FMS(spu_opcode_t op);
};
