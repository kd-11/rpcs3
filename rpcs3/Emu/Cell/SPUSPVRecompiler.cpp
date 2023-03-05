#include "stdafx.h"
#include "SPUSPVRecompiler.h"

#include "Emu/IdManager.h"
#include "Emu/system_config.h"
#include "Crypto/sha1.h"

#include "Emu/RSX/rsx_utils.h"

#include <windows.h>
#pragma optimize("", off)

namespace
{
	const spu_decoder<spv_recompiler> s_spu_decoder;

	const auto println = [](const char* s)
	{
		char buf[512];
		snprintf(buf, 512, "[spvc] %s\n", s);
		rsx_log.error(buf);
	};

	void spu_spv_entry(spu_thread& /*thread*/, void* /*compiled function*/, u8*)
	{
		println("SPV function executed");
	}
}

std::unique_ptr<spu_recompiler_base> spu_recompiler_base::make_spv_recompiler()
{
	return std::make_unique<spv_recompiler>();
}

spv_recompiler::spv_recompiler()
{
}

void spv_recompiler::init()
{
	// Initialize if necessary
	if (!m_spurt)
	{
		m_spurt = &g_fxo->get<spu_runtime>();
	}
}

spu_function_t spv_recompiler::compile(spu_program&& _func)
{
	const u32 start0 = _func.entry_point;

	const auto add_loc = m_spurt->add_empty(std::move(_func));

	if (!add_loc)
	{
		return nullptr;
	}

	if (add_loc->compiled)
	{
		return add_loc->compiled;
	}

	const spu_program& func = add_loc->data;

	if (func.entry_point != start0)
	{
		// Wait for the duplicate
		while (!add_loc->compiled)
		{
			add_loc->compiled.wait(nullptr);
		}

		return add_loc->compiled;
	}

	if (auto& cache = g_fxo->get<spu_cache>(); cache && g_cfg.core.spu_cache && !add_loc->cached.exchange(1))
	{
		cache.add(func);
	}

	{
		sha1_context ctx;
		u8 output[20];

		sha1_starts(&ctx);
		sha1_update(&ctx, reinterpret_cast<const u8*>(func.data.data()), func.data.size() * 4);
		sha1_finish(&ctx, output);

		be_t<u64> hash_start;
		std::memcpy(&hash_start, output, sizeof(hash_start));
		m_hash_start = hash_start;
	}

	m_pos = func.lower_bound;
	u32 m_base = func.entry_point;
	m_size = ::size32(func.data) * 4;
	const u32 start = m_pos;
	const u32 end = start + m_size;

	// Clear
	c.reset();

	// Create block labels

	// Get bit mask of valid code words for a given range (up to 128 bytes)
	auto get_code_mask = [&](u32 starta, u32 enda) -> u32
	{
		u32 result = 0;

		for (u32 addr = starta, m = 1; addr < enda && m; addr += 4, m <<= 1)
		{
			// Filter out if out of range, or is a hole
			if (addr >= start && addr < end && func.data[(addr - start) / 4])
			{
				result |= m;
			}
		}

		return result;
	};

	// Check code
	u32 starta = start;

	// Skip holes at the beginning (giga only)
	for (u32 j = start; j < end; j += 4)
	{
		if (!func.data[(j - start) / 4])
		{
			starta += 4;
		}
		else
		{
			break;
		}
	}

	// TODO - Init compiler state
	// Clear registers, etc

	if (m_pos != start)
	{
		// Jump to the entry point if necessary
	}

	for (u32 i = 0; i < func.data.size(); i++)
	{
		const u32 pos = start + i * 4;
		const u32 op = std::bit_cast<be_t<u32>>(func.data[i]);

		if (!op)
		{
			// Ignore hole
			if (m_pos + 1)
			{
				spu_log.error("Unexpected fallthrough to 0x%x", pos);
				// TODO
				// branch_fixed(spu_branch_target(pos));
				m_pos = -1;
			}

			continue;
		}

		// Update position
		m_pos = pos;

		// Bind instruction label if necessary
		// const auto found = instr_labels.find(pos);

		// Tracing
		//c->lea(x86::r14, get_pc(m_pos));

		// Execute recompiler function
		(this->*s_spu_decoder.decode(op))({ op });
	}

	// Make fallthrough if necessary
	if (m_pos + 1)
	{
		// TODO
		// branch_fixed(spu_branch_target(end));
	}

	// Epilogue

	// Build instruction dispatch table


	// Compile and get function address
	spu_function_t fn = reinterpret_cast<spu_function_t>(spu_spv_entry);
	c.compile();
	/*
	spu_function_t fn = reinterpret_cast<spu_function_t>(m_asmrt._add(&code));

	if (!fn)
	{
		spu_log.fatal("Failed to build a function");
	}
	else
	{
		jit_announce(fn, code.codeSize(), fmt::format("spu-b-%s", fmt::base57(be_t<u64>(m_hash_start))));
	}*/

	// Install compiled function pointer
	const bool added = !add_loc->compiled && add_loc->compiled.compare_and_swap_test(nullptr, fn);

	if (added)
	{
		add_loc->compiled.notify_all();
	}

	return fn;
}

void spv_recompiler::UNK(spu_opcode_t op)
{
	println("UNK");
}

void spv_recompiler::STOP(spu_opcode_t op)
{
	println("STOP");
}

void spv_recompiler::LNOP(spu_opcode_t op)
{
	println("LNOP");
}

void spv_recompiler::SYNC(spu_opcode_t op)
{
	println("SYNC");
}

void spv_recompiler::DSYNC(spu_opcode_t op)
{
	println("DSYNC");
}

void spv_recompiler::MFSPR(spu_opcode_t op)
{
	println("MFSPR");
}

void spv_recompiler::RDCH(spu_opcode_t op)
{
	println("RDCH");
}

void spv_recompiler::RCHCNT(spu_opcode_t op)
{
	println("RCHCNT");
}

void spv_recompiler::SF(spu_opcode_t op)
{
	c.v_subs(op.rt, op.rb, op.ra);
}

void spv_recompiler::OR(spu_opcode_t op)
{
	println("OR");
}

void spv_recompiler::BG(spu_opcode_t op)
{
	println("BG");
}

void spv_recompiler::SFH(spu_opcode_t op)
{
	println("SFH");
}

void spv_recompiler::NOR(spu_opcode_t op)
{
	println("NOR");
}

void spv_recompiler::ABSDB(spu_opcode_t op)
{
	println("ABSDB");
}

void spv_recompiler::ROT(spu_opcode_t op)
{
	println("ROT");
}

void spv_recompiler::ROTM(spu_opcode_t op)
{
	println("ROTM");
}

void spv_recompiler::ROTMA(spu_opcode_t op)
{
	println("ROTMA");
}

void spv_recompiler::SHL(spu_opcode_t op)
{
	println("SHL");
}

void spv_recompiler::ROTH(spu_opcode_t op)
{
	println("ROTH");
}

void spv_recompiler::ROTHM(spu_opcode_t op)
{
	println("ROTHM");
}

void spv_recompiler::ROTMAH(spu_opcode_t op)
{
	println("ROTMAH");
}

void spv_recompiler::SHLH(spu_opcode_t op)
{
	println("SHLH");
}

void spv_recompiler::ROTI(spu_opcode_t op)
{
	println("ROTI");
}

void spv_recompiler::ROTMI(spu_opcode_t op)
{
	println("ROTMI");
}

void spv_recompiler::ROTMAI(spu_opcode_t op)
{
	println("ROTMAI");
}

void spv_recompiler::SHLI(spu_opcode_t op)
{
	println("SHLI");
}

void spv_recompiler::ROTHI(spu_opcode_t op)
{
	println("ROTHI");
}

void spv_recompiler::ROTHMI(spu_opcode_t op)
{
	println("ROTHMI");
}

void spv_recompiler::ROTMAHI(spu_opcode_t op)
{
	println("ROTMAHI");
}

void spv_recompiler::SHLHI(spu_opcode_t op)
{
	println("SHLHI");
}

void spv_recompiler::A(spu_opcode_t op)
{
	println("A");
}

void spv_recompiler::AND(spu_opcode_t op)
{
	println("AND");
}

void spv_recompiler::CG(spu_opcode_t op)
{
	println("CG");
}

void spv_recompiler::AH(spu_opcode_t op)
{
	println("AH");
}

void spv_recompiler::NAND(spu_opcode_t op)
{
	println("NAND");
}

void spv_recompiler::AVGB(spu_opcode_t op)
{
	println("AVGB");
}

void spv_recompiler::MTSPR(spu_opcode_t op)
{
	println("MTSPR");
}

void spv_recompiler::WRCH(spu_opcode_t op)
{
	println("WRCH");
}

void spv_recompiler::BIZ(spu_opcode_t op)
{
	println("BIZ");
}

void spv_recompiler::BINZ(spu_opcode_t op)
{
	println("BINZ");
}

void spv_recompiler::BIHZ(spu_opcode_t op)
{
	println("BIHZ");
}

void spv_recompiler::BIHNZ(spu_opcode_t op)
{
	println("BIHNZ");
}

void spv_recompiler::STOPD(spu_opcode_t op)
{
	println("STOPD");
}

void spv_recompiler::STQX(spu_opcode_t op)
{
	println("STQX");
}

void spv_recompiler::BI(spu_opcode_t op)
{
	println("BI");
}

void spv_recompiler::BISL(spu_opcode_t op)
{
	println("BISL");
}

void spv_recompiler::IRET(spu_opcode_t op)
{
	println("IRET");
}

void spv_recompiler::BISLED(spu_opcode_t op)
{
	println("BISLED");
}

void spv_recompiler::HBR(spu_opcode_t op)
{
	println("HBR");
}

void spv_recompiler::GB(spu_opcode_t op)
{
	println("GB");
}

void spv_recompiler::GBH(spu_opcode_t op)
{
	println("GBH");
}

void spv_recompiler::GBB(spu_opcode_t op)
{
	println("GBB");
}

void spv_recompiler::FSM(spu_opcode_t op)
{
	println("FSM");
}

void spv_recompiler::FSMH(spu_opcode_t op)
{
	println("FSMH");
}

void spv_recompiler::FSMB(spu_opcode_t op)
{
	println("FSMB");
}

void spv_recompiler::FREST(spu_opcode_t op)
{
	println("FREST");
}

void spv_recompiler::FRSQEST(spu_opcode_t op)
{
	println("FRSQEST");
}

void spv_recompiler::LQX(spu_opcode_t op)
{
	println("LQX");
}

void spv_recompiler::ROTQBYBI(spu_opcode_t op)
{
	println("ROTQBYBI");
}

void spv_recompiler::ROTQMBYBI(spu_opcode_t op)
{
	println("ROTQMBYBI");
}

void spv_recompiler::SHLQBYBI(spu_opcode_t op)
{
	println("SHLQBYBI");
}

void spv_recompiler::CBX(spu_opcode_t op)
{
	println("CBX");
}

void spv_recompiler::CHX(spu_opcode_t op)
{
	println("CHX");
}

void spv_recompiler::CWX(spu_opcode_t op)
{
	println("CWX");
}

void spv_recompiler::CDX(spu_opcode_t op)
{
	println("CDX");
}

void spv_recompiler::ROTQBI(spu_opcode_t op)
{
	println("ROTQBI");
}

void spv_recompiler::ROTQMBI(spu_opcode_t op)
{
	println("ROTQMBI");
}

void spv_recompiler::SHLQBI(spu_opcode_t op)
{
	println("SHLQBI");
}

void spv_recompiler::ROTQBY(spu_opcode_t op)
{
	println("ROTQBY");
}

void spv_recompiler::ROTQMBY(spu_opcode_t op)
{
	println("ROTQMBY");
}

void spv_recompiler::SHLQBY(spu_opcode_t op)
{
	println("SHLQBY");
}

void spv_recompiler::ORX(spu_opcode_t op)
{
	println("ORX");
}

void spv_recompiler::CBD(spu_opcode_t op)
{
	println("CBD");
}

void spv_recompiler::CHD(spu_opcode_t op)
{
	println("CHD");
}

void spv_recompiler::CWD(spu_opcode_t op)
{
	println("CWD");
}

void spv_recompiler::CDD(spu_opcode_t op)
{
	println("CDD");
}

void spv_recompiler::ROTQBII(spu_opcode_t op)
{
	println("ROTQBII");
}

void spv_recompiler::ROTQMBII(spu_opcode_t op)
{
	println("ROTQMBII");
}

void spv_recompiler::SHLQBII(spu_opcode_t op)
{
	println("SHLQBII");
}

void spv_recompiler::ROTQBYI(spu_opcode_t op)
{
	const auto shift_distance = (op.i7 & 0xf);
	c.v_shlsi(op.rt, op.ra, spv_constant::spread(shift_distance));
	c.v_bfxsi(c.v_tmp0, op.ra, spv_constant::make_si(31), spv_constant::make_si(shift_distance));
	c.v_ors(op.rt, c.v_tmp0, op.rt);
}

void spv_recompiler::ROTQMBYI(spu_opcode_t op)
{
	println("ROTQMBYI");
}

void spv_recompiler::SHLQBYI(spu_opcode_t op)
{
	println("SHLQBYI");
}

void spv_recompiler::NOP(spu_opcode_t op)
{
	println("NOP");
}

void spv_recompiler::CGT(spu_opcode_t op)
{
	println("CGT");
}

void spv_recompiler::XOR(spu_opcode_t op)
{
	println("XOR");
}

void spv_recompiler::CGTH(spu_opcode_t op)
{
	println("CGTH");
}

void spv_recompiler::EQV(spu_opcode_t op)
{
	println("EQV");
}

void spv_recompiler::CGTB(spu_opcode_t op)
{
	println("CGTB");
}

void spv_recompiler::SUMB(spu_opcode_t op)
{
	println("SUMB");
}

void spv_recompiler::HGT(spu_opcode_t op)
{
	println("HGT");
}

void spv_recompiler::CLZ(spu_opcode_t op)
{
	println("CLZ");
}

void spv_recompiler::XSWD(spu_opcode_t op)
{
	println("XSWD");
}

void spv_recompiler::XSHW(spu_opcode_t op)
{
	println("XSHW");
}

void spv_recompiler::CNTB(spu_opcode_t op)
{
	println("CNTB");
}

void spv_recompiler::XSBH(spu_opcode_t op)
{
	println("XSBH");
}

void spv_recompiler::CLGT(spu_opcode_t op)
{
	println("CLGT");
}

void spv_recompiler::ANDC(spu_opcode_t op)
{
	println("ANDC");
}

void spv_recompiler::FCGT(spu_opcode_t op)
{
	println("FCGT");
}

void spv_recompiler::DFCGT(spu_opcode_t op)
{
	println("DFCGT");
}

void spv_recompiler::FA(spu_opcode_t op)
{
	println("FA");
}

void spv_recompiler::FS(spu_opcode_t op)
{
	println("FS");
}

void spv_recompiler::FM(spu_opcode_t op)
{
	println("FM");
}

void spv_recompiler::CLGTH(spu_opcode_t op)
{
	println("CLGTH");
}

void spv_recompiler::ORC(spu_opcode_t op)
{
	println("ORC");
}

void spv_recompiler::FCMGT(spu_opcode_t op)
{
	println("FCMGT");
}

void spv_recompiler::DFCMGT(spu_opcode_t op)
{
	println("DFCMGT");
}

void spv_recompiler::DFA(spu_opcode_t op)
{
	println("DFA");
}

void spv_recompiler::DFS(spu_opcode_t op)
{
	println("DFS");
}

void spv_recompiler::DFM(spu_opcode_t op)
{
	println("DFM");
}

void spv_recompiler::CLGTB(spu_opcode_t op)
{
	println("CLGTB");
}

void spv_recompiler::HLGT(spu_opcode_t op)
{
	println("HLGT");
}

void spv_recompiler::DFMA(spu_opcode_t op)
{
	println("DFMA");
}

void spv_recompiler::DFMS(spu_opcode_t op)
{
	println("DFMS");
}

void spv_recompiler::DFNMS(spu_opcode_t op)
{
	println("DFNMS");
}

void spv_recompiler::DFNMA(spu_opcode_t op)
{
	println("DFNMA");
}

void spv_recompiler::CEQ(spu_opcode_t op)
{
	println("CEQ");
}

void spv_recompiler::MPYHHU(spu_opcode_t op)
{
	println("MPYHHU");
}

void spv_recompiler::ADDX(spu_opcode_t op)
{
	println("ADDX");
}

void spv_recompiler::SFX(spu_opcode_t op)
{
	println("SFX");
}

void spv_recompiler::CGX(spu_opcode_t op)
{
	println("CGX");
}

void spv_recompiler::BGX(spu_opcode_t op)
{
	println("BGX");
}

void spv_recompiler::MPYHHA(spu_opcode_t op)
{
	println("MPYHHA");
}

void spv_recompiler::MPYHHAU(spu_opcode_t op)
{
	println("MPYHHAU");
}

void spv_recompiler::FSCRRD(spu_opcode_t op)
{
	println("FSCRRD");
}

void spv_recompiler::FESD(spu_opcode_t op)
{
	println("FESD");
}

void spv_recompiler::FRDS(spu_opcode_t op)
{
	println("FRDS");
}

void spv_recompiler::FSCRWR(spu_opcode_t op)
{
	println("FSCRWR");
}

void spv_recompiler::DFTSV(spu_opcode_t op)
{
	println("DFTSV");
}

void spv_recompiler::FCEQ(spu_opcode_t op)
{
	println("FCEQ");
}

void spv_recompiler::DFCEQ(spu_opcode_t op)
{
	println("DFCEQ");
}

void spv_recompiler::MPY(spu_opcode_t op)
{
	println("MPY");
}

void spv_recompiler::MPYH(spu_opcode_t op)
{
	println("MPYH");
}

void spv_recompiler::MPYHH(spu_opcode_t op)
{
	println("MPYHH");
}

void spv_recompiler::MPYS(spu_opcode_t op)
{
	println("MPYS");
}

void spv_recompiler::CEQH(spu_opcode_t op)
{
	println("CEQH");
}

void spv_recompiler::FCMEQ(spu_opcode_t op)
{
	println("FCMEQ");
}

void spv_recompiler::DFCMEQ(spu_opcode_t op)
{
	println("DFCMEQ");
}

void spv_recompiler::MPYU(spu_opcode_t op)
{
	println("MPYU");
}

void spv_recompiler::CEQB(spu_opcode_t op)
{
	println("CEQB");
}

void spv_recompiler::FI(spu_opcode_t op)
{
	println("FI");
}

void spv_recompiler::HEQ(spu_opcode_t op)
{
	println("HEQ");
}

void spv_recompiler::CFLTS(spu_opcode_t op)
{
	println("CFLTS");
}

void spv_recompiler::CFLTU(spu_opcode_t op)
{
	println("CFLTU");
}

void spv_recompiler::CSFLT(spu_opcode_t op)
{
	println("CSFLT");
}

void spv_recompiler::CUFLT(spu_opcode_t op)
{
	println("CUFLT");
}

void spv_recompiler::BRZ(spu_opcode_t op)
{
	println("BRZ");
}

void spv_recompiler::STQA(spu_opcode_t op)
{
	println("STQA");
}

void spv_recompiler::BRNZ(spu_opcode_t op)
{
	println("BRNZ");
}

void spv_recompiler::BRHZ(spu_opcode_t op)
{
	println("BRHZ");
}

void spv_recompiler::BRHNZ(spu_opcode_t op)
{
	println("BRHNZ");
}

void spv_recompiler::STQR(spu_opcode_t op)
{
	const auto lsa = spu_ls_target(m_pos, op.i16);
	c.v_storq(spv_constant::make_su(lsa), op.rt);
}

void spv_recompiler::BRA(spu_opcode_t op)
{
	println("BRA");
}

void spv_recompiler::LQA(spu_opcode_t op)
{
	println("LQA");
}

void spv_recompiler::BRASL(spu_opcode_t op)
{
	println("BRASL");
}

void spv_recompiler::BR(spu_opcode_t op)
{
	println("BR");
}

void spv_recompiler::FSMBI(spu_opcode_t op)
{
	// Compile-time complexity, no need to micro-optimize the twiddle
	u8 result[16];
	for (int bit = 0; bit < 16; ++bit)
	{
		result[bit] = (op.i16 & (1 << bit)) ? 0xFF : 0;
	}

	c.v_movsi(op.rt, spv::vector_const_t(result).as_vi());
}

void spv_recompiler::BRSL(spu_opcode_t op)
{
	const auto target = spu_branch_target(m_pos, op.i16);
	c.v_movsi(op.rt, spv_constant::make_vi(static_cast<s32>(target)));
	c.s_jmp(spv_constant::make_su(target));

	if (target != m_pos + 4)
	{
		// Fall out
		m_pos = -1;
	}
}

void spv_recompiler::LQR(spu_opcode_t op)
{
	println("LQR");
}

void spv_recompiler::IL(spu_opcode_t op)
{
	const auto var = spv_constant::spread(op.si16);
	c.v_movsi(op.rt, var);
}

void spv_recompiler::ILHU(spu_opcode_t op)
{
	println("ILHU");
}

void spv_recompiler::ILH(spu_opcode_t op)
{
	println("ILH");
}

void spv_recompiler::IOHL(spu_opcode_t op)
{
	println("IOHL");
}

void spv_recompiler::ORI(spu_opcode_t op)
{
	const auto var = spv_constant::spread(op.si10);
	c.v_orsi(op.rt, op.ra, var);
}

void spv_recompiler::ORHI(spu_opcode_t op)
{
	println("ORHI");
}

void spv_recompiler::ORBI(spu_opcode_t op)
{
	println("ORBI");
}

void spv_recompiler::SFI(spu_opcode_t op)
{
	println("SFI");
}

void spv_recompiler::SFHI(spu_opcode_t op)
{
	println("SFHI");
}

void spv_recompiler::ANDI(spu_opcode_t op)
{
	println("ANDI");
}

void spv_recompiler::ANDHI(spu_opcode_t op)
{
	println("ANDHI");
}

void spv_recompiler::ANDBI(spu_opcode_t op)
{
	println("ANDBI");
}

void spv_recompiler::AI(spu_opcode_t op)
{
	c.v_addsi(op.rt, op.ra, spv_constant::spread(op.si10));
}

void spv_recompiler::AHI(spu_opcode_t op)
{
	println("AHI");
}

void spv_recompiler::STQD(spu_opcode_t op)
{
	const auto si10 = spv_constant::make_si(op.si10 * 16);
	const auto address_mask = spv_constant::make_si(0x3fff0);
	c.s_xtrs(c.s_tmp0, op.ra, 3);
	c.s_addsi(c.s_tmp0, c.s_tmp0, si10);
	c.s_andsi(c.s_tmp0, c.s_tmp0, address_mask);
	c.v_storq(c.s_tmp0, op.rt);
}

void spv_recompiler::LQD(spu_opcode_t op)
{
	println("LQD");
}

void spv_recompiler::XORI(spu_opcode_t op)
{
	println("XORI");
}

void spv_recompiler::XORHI(spu_opcode_t op)
{
	println("XORHI");
}

void spv_recompiler::XORBI(spu_opcode_t op)
{
	println("XORBI");
}

void spv_recompiler::CGTI(spu_opcode_t op)
{
	println("CGTI");
}

void spv_recompiler::CGTHI(spu_opcode_t op)
{
	println("CGTHI");
}

void spv_recompiler::CGTBI(spu_opcode_t op)
{
	println("CGTBI");
}

void spv_recompiler::HGTI(spu_opcode_t op)
{
	println("HGTI");
}

void spv_recompiler::CLGTI(spu_opcode_t op)
{
	println("CLGTI");
}

void spv_recompiler::CLGTHI(spu_opcode_t op)
{
	println("CLGTHI");
}

void spv_recompiler::CLGTBI(spu_opcode_t op)
{
	println("CLGTBI");
}

void spv_recompiler::HLGTI(spu_opcode_t op)
{
	println("HLGTI");
}

void spv_recompiler::MPYI(spu_opcode_t op)
{
	println("MPYI");
}

void spv_recompiler::MPYUI(spu_opcode_t op)
{
	println("MPYUI");
}

void spv_recompiler::CEQI(spu_opcode_t op)
{
	const auto imm = spv_constant::spread(op.si10);
	c.v_cmpeqsi(op.rt, op.ra, imm);
}

void spv_recompiler::CEQHI(spu_opcode_t op)
{
	println("CEQHI");
}

void spv_recompiler::CEQBI(spu_opcode_t op)
{
	println("CEQBI");
}

void spv_recompiler::HEQI(spu_opcode_t op)
{
	println("HEQI");
}

void spv_recompiler::HBRA(spu_opcode_t op)
{
	println("HBRA");
}

void spv_recompiler::HBRR(spu_opcode_t op)
{
	println("HBRR");
}

void spv_recompiler::ILA(spu_opcode_t op)
{
	const auto var = spv_constant::spread(op.i18).as_vi();
	c.v_movsi(op.rt, var);
}

void spv_recompiler::SELB(spu_opcode_t op)
{
	c.v_xorsi(c.v_tmp0, op.rc, spv_constant::spread(0));
	c.v_ands(c.v_tmp1, op.rc, op.rb);
	c.v_ands(c.v_tmp0, c.v_tmp0, op.ra);
	c.v_ors(op.rt, c.v_tmp0, c.v_tmp1);
}

void spv_recompiler::SHUFB(spu_opcode_t op)
{
	println("SHUFB");
}

void spv_recompiler::MPYA(spu_opcode_t op)
{
	println("MPYA");
}

void spv_recompiler::FNMS(spu_opcode_t op)
{
	println("FNMS");
}

void spv_recompiler::FMA(spu_opcode_t op)
{
	println("FMA");
}

void spv_recompiler::FMS(spu_opcode_t op)
{
	println("FMS");
}

namespace spv_constant
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
};

// Management
void spv_emitter::reset()
{
	m_block.clear();
	m_v_const_array.clear();
	m_s_const_array.clear();
}

void spv_emitter::compile()
{
	std::stringstream shaderbuf;
	// Declare common
	shaderbuf <<
		"#version 460\n"
		"layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
		"\n"
		"layout(set=0, binding=0, std430) buffer local_storage { ivec4 lsa[16384]; }\n"
		"layout(set=0, binding=1, std430) buffer register_file { ivec4 vgpr[144]; }\n"
		"layout(set=0, binding=2, std430) buffer control_block { int pc; }\n"
		"\n"
		"// Temp registers\n"
		"vec4 vgprf[2];\n"
		"int sgpr[4];\n"
		"\n";

	// TODO: Cache/mirror registers

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
						"vec4(%f, %f, %f, %f)",
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
						"{ vec4(%f, %f, %f, %f), vec4(%f, %f, %f, %f) };\n",
						rsx::decode_fp16(const_.value.h[0]), rsx::decode_fp16(const_.value.h[1]), rsx::decode_fp16(const_.value.h[2]), rsx::decode_fp16(const_.value.h[3]),
						rsx::decode_fp16(const_.value.h[4]), rsx::decode_fp16(const_.value.h[5]), rsx::decode_fp16(const_.value.h[6]), rsx::decode_fp16(const_.value.h[7]));
					return { "vec4", "[2]", initializer};
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
		const auto [declared_type, value] = [](const spv::scalar_const_t& const_) -> std::tuple<std::string_view,  std::string>
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

	// Body
	shaderbuf <<
		"void execute()\n"
		"{\n" <<
			m_block <<
		"}\n\n";

	// Entry
	// TODO: Retire/flush cache registers
	shaderbuf <<
		"void main()\n"
		"{\n"
		"	execute();\n"
		"}\n\n";

	// TODO
	m_block = shaderbuf.str();
	println(m_block.data());
	reset();
}

// Arithmetic ops
void spv_emitter::v_addsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] + %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_subs(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] - vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::s_addsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] + %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_adds(spv::scalar_register_t dst, spv::scalar_register_t op0, spv::scalar_register_t op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] + sgpr[%d];\n",
		dst.sgpr_index, op0.sgpr_index, op1.sgpr_index);
}

// Comparison
void spv_emitter::v_cmpeqsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = ivec4(equal(vgpr[%d], %s));\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

// Movs
void spv_emitter::v_movsi(spv::vector_register_t dst, const spv::vector_const_t& src)
{
	m_block += fmt::format(
		"vgpr[%d] = %s;\n",
		dst.vgpr_index, get_const_name(src));
}

void spv_emitter::v_movfi(spv::vector_register_t dst, const spv::vector_const_t& src)
{
	m_block += fmt::format(
		"vgpr[%d] = floatBitsToInt(%s);\n",
		dst.vgpr_index, get_const_name(src));
}

void spv_emitter::v_storq(spv::scalar_register_t lsa, spv::vector_register_t src_reg)
{
	m_block += fmt::format(
		"lsa[sgpr[%d] >> 2] = vgpr[%d];\n",
		lsa.sgpr_index, src_reg.vgpr_index);
}

void spv_emitter::v_storq(spv::scalar_const_t lsa, spv::vector_register_t src_reg)
{
	m_block += fmt::format(
		"lsa[%s >> 2] = vgpr[%d];\n",
		get_const_name(lsa), src_reg.vgpr_index);
}

// Bitwise
void spv_emitter::v_ands(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] & vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::v_bfxsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::scalar_const_t& op1, const spv::scalar_const_t& op2)
{
	m_block += fmt::format(
		"vgpr[%d] = bitfieldExtract(vgpr[%d], %s, %s);\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1), get_const_name(op2));
}

void spv_emitter::v_shlsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] << %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_shrsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] >> %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_xorsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] ^ %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_orsi(spv::vector_register_t dst, spv::vector_register_t op0, const spv::vector_const_t& op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] | %s;\n",
		dst.vgpr_index, op0.vgpr_index, get_const_name(op1));
}

void spv_emitter::v_ors(spv::vector_register_t dst, spv::vector_register_t op0, spv::vector_register_t op1)
{
	m_block += fmt::format(
		"vgpr[%d] = vgpr[%d] | vgpr[%d];\n",
		dst.vgpr_index, op0.vgpr_index, op1.vgpr_index);
}

void spv_emitter::s_andsi(spv::scalar_register_t dst, spv::scalar_register_t op0, const spv::scalar_const_t& op1)
{
	m_block += fmt::format(
		"sgpr[%d] = sgpr[%d] & %s;\n",
		dst.sgpr_index, op0.sgpr_index, get_const_name(op1));
}

void spv_emitter::s_xtrs(spv::scalar_register_t dst, spv::vector_register_t src, int component)
{
	m_block += fmt::format(
		"sgpr[%d] = vgpr[%d][%d];\n",
		dst.sgpr_index, src.vgpr_index, component);
}

void spv_emitter::s_ins(spv::vector_register_t dst, spv::scalar_register_t src, int component)
{
	m_block += fmt::format(
		"vgpr[%d][%d] = sgpr[%d];\n",
		dst.vgpr_index, component, src.sgpr_index);
}

// Flow control
void spv_emitter::s_jmp(const spv::scalar_const_t& target)
{
	m_block += fmt::format(
		"pc = %s;\n",
		get_const_name(target.as_u()));
}

void spv_emitter::s_jmp(spv::scalar_register_t target)
{
	m_block += fmt::format(
		"pc = sgpr[%d];\n",
		target.sgpr_index);
}

std::string spv_emitter::get_const_name(const spv::vector_const_t& const_)
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

std::string spv_emitter::get_const_name(const spv::scalar_const_t& const_)
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
