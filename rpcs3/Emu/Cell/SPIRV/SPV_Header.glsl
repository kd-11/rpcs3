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
	ivec4 qshl_mask_lookup[128];
	ivec4 qshr_mask_lookup[128];
};

// Temp registers
vec4 vgprf[2];
int sgpr[4];

// Standard definitions
#define SPU_HLT     1
#define SPU_MFC_CMD 2

// Standard wrappers
ivec4 _bswap(const in ivec4 reg)
{
	const ivec4 a = bitfieldExtract(reg, 0, 8);
	const ivec4 b = bitfieldExtract(reg, 8, 8);
	const ivec4 c = bitfieldExtract(reg, 16, 8);
	const ivec4 d = bitfieldExtract(reg, 24, 8);
	const ivec4 e = d | (c << 8) | (b << 16) | (a << 24);
	return e.wzyx;
}
)"
