R"(
#version 460
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set=0, binding=0, std430) restrict buffer local_storage { ivec4 ls[16384]; };
layout(set=0, binding=1, std430) restrict buffer register_file
{
	// GPR file
	ivec4 vgpr[144];

	// System registers
	int srr0;
	int cflags;
	int MFC_tag_mask;
	int MFC_tag_stat_count;
	int MFC_tag_stat_value;
	int MFC_tag_update;
	int MFC_tag_id;
	int MFC_lsa;
	int MFC_eal;
	int MFC_eah;
	int MFC_size;
	int MFC_cmd_id;
	int MFC_fence;
};

layout(set=0, binding=2, std430) writeonly restrict buffer control_block
{
	// Thread control and debugging/inspection
	uint pc;
	int exit_code;
	int lr;
	int sp;
	int dr[16];
};

layout(set=0, binding=3, std430) readonly restrict buffer constants_block
{
	// Static constants
	ivec4 qshr_mask_lookup[128];
	ivec4 qshl_mask_lookup[128];
};

// Temp registers
vec4 vgprf[2];
uvec4 vgpru[2];
int sgpr[4];

// Standard definitions
#define SPU_SUCCESS         0
#define SPU_HLT             1
#define SPU_MFC_CMD         2
#define SPU_RDCH_SigNotify1 3

// Standard wrappers
ivec4 _bswap(const in ivec4 reg)
{
	const uvec4 ureg = uvec4(reg);
	const uvec4 a = bitfieldExtract(ureg, 0, 8);
	const uvec4 b = bitfieldExtract(ureg, 8, 8);
	const uvec4 c = bitfieldExtract(ureg, 16, 8);
	const uvec4 d = bitfieldExtract(ureg, 24, 8);
	const uvec4 e = d | (c << 8) | (b << 16) | (a << 24);
	return ivec4(e.wzyx);
}

// Workaround for signed bfe being fucked
#define _vbfe(v, o, c) ivec4(bitfieldExtract(uvec4(v), o, c))

)"
