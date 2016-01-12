#include "stdafx.h"
#include "ProgramStateCache.h"

using namespace program_hash_util;

size_t vertex_program_hash::operator()(const std::vector<u32> &program) const
{
	// 64-bit Fowler/Noll/Vo FNV-1a hash code
	size_t hash = 0xCBF29CE484222325ULL;
	const qword *instbuffer = (const qword*)program.data();
	size_t instIndex = 0;
	bool end = false;
	for (unsigned i = 0; i < program.size() / 4; i++)
	{
		const qword inst = instbuffer[instIndex];
		hash ^= inst.dword[0];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		hash ^= inst.dword[1];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		instIndex++;
	}
	return hash;
}

bool vertex_program_compare::operator()(const std::vector<u32> &binary1, const std::vector<u32> &binary2) const
{
	if (binary1.size() != binary2.size()) return false;
	const qword *instBuffer1 = (const qword*)binary1.data();
	const qword *instBuffer2 = (const qword*)binary2.data();
	size_t instIndex = 0;
	for (unsigned i = 0; i < binary1.size() / 4; i++)
	{
		const qword& inst1 = instBuffer1[instIndex];
		const qword& inst2 = instBuffer2[instIndex];
		if (inst1.dword[0] != inst2.dword[0] || inst1.dword[1] != inst2.dword[1])
			return false;
		instIndex++;
	}
	return true;
}


bool fragment_program_utils::is_constant(u32 sourceOperand)
{
	return ((sourceOperand >> 8) & 0x3) == 2;
}

size_t fragment_program_utils::get_fragment_program_ucode_size(void *ptr)
{
	const qword *instBuffer = (const qword*)ptr;
	size_t instIndex = 0;
	while (true)
	{
		const qword& inst = instBuffer[instIndex];
		bool isSRC0Constant = is_constant(inst.word[1]);
		bool isSRC1Constant = is_constant(inst.word[2]);
		bool isSRC2Constant = is_constant(inst.word[3]);
		bool end = (inst.word[0] >> 8) & 0x1;

		if (isSRC0Constant || isSRC1Constant || isSRC2Constant)
		{
			instIndex += 2;
			if (end)
				return instIndex * 4 * 4;
			continue;
		}
		instIndex++;
		if (end)
			return (instIndex)* 4 * 4;
	}
}

size_t fragment_program_hash::operator()(const void *program) const
{
	// 64-bit Fowler/Noll/Vo FNV-1a hash code
	size_t hash = 0xCBF29CE484222325ULL;
	const qword *instbuffer = (const qword*)program;
	size_t instIndex = 0;
	while (true)
	{
		const qword& inst = instbuffer[instIndex];
		hash ^= inst.dword[0];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		hash ^= inst.dword[1];
		hash += (hash << 1) + (hash << 4) + (hash << 5) + (hash << 7) + (hash << 8) + (hash << 40);
		instIndex++;
		// Skip constants
		if (fragment_program_utils::is_constant(inst.word[1]) ||
			fragment_program_utils::is_constant(inst.word[2]) ||
			fragment_program_utils::is_constant(inst.word[3]))
			instIndex++;

		bool end = (inst.word[0] >> 8) & 0x1;
		if (end)
			return hash;
	}
	return 0;
}

bool fragment_program_compare::operator()(const void *binary1, const void *binary2) const
{
	const qword *instBuffer1 = (const qword*)binary1;
	const qword *instBuffer2 = (const qword*)binary2;
	size_t instIndex = 0;
	while (true)
	{
		const qword& inst1 = instBuffer1[instIndex];
		const qword& inst2 = instBuffer2[instIndex];

		if (inst1.dword[0] != inst2.dword[0] || inst1.dword[1] != inst2.dword[1])
			return false;
		instIndex++;
		// Skip constants
		if (fragment_program_utils::is_constant(inst1.word[1]) ||
			fragment_program_utils::is_constant(inst1.word[2]) ||
			fragment_program_utils::is_constant(inst1.word[3]))
			instIndex++;

		bool end = ((inst1.word[0] >> 8) & 0x1) && ((inst2.word[0] >> 8) & 0x1);
		if (end)
			return true;
	}
}
