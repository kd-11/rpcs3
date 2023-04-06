R"(
void v128_to_bytes(const in uvec4 v, out uint[16] bytes)
{
	const uvec4 v0 = bitfieldExtract(v, 0, 8);
	const uvec4 v1 = bitfieldExtract(v, 8, 8);
	const uvec4 v2 = bitfieldExtract(v, 16, 8);
	const uvec4 v3 = bitfieldExtract(v, 24, 8);

	bytes[0] = v0.x;
	bytes[1] = v1.x;
	bytes[2] = v2.x;
	bytes[3] = v3.x;

	bytes[4] = v0.y;
	bytes[5] = v1.y;
	bytes[6] = v2.y;
	bytes[7] = v3.y;

	bytes[8] = v0.z;
	bytes[9] = v1.z;
	bytes[10] = v2.z;
	bytes[11] = v3.z;

	bytes[12] = v0.w;
	bytes[13] = v1.w;
	bytes[14] = v2.w;
	bytes[15] = v3.w;
}

uvec4 v128_from_bytes(const in uint[16] bytes)
{
	// Essentially a matrix multiply using shifts and ors
	const uvec4 v0 = uvec4(bytes[0], bytes[4], bytes[8], bytes[12]);
	const uvec4 v1 = uvec4(bytes[1], bytes[5], bytes[9], bytes[13]);
	const uvec4 v2 = uvec4(bytes[2], bytes[6], bytes[10], bytes[14]);
	const uvec4 v3 = uvec4(bytes[3], bytes[7], bytes[11], bytes[15]);

	return v0 | (v1 << 8) | (v2 << 16) | (v3 << 24);
}

ivec4 _dqshufb(const in ivec4 a, const in ivec4 b, const in ivec4 c)
{
	// Variables
	uint[16] sources[2]; // 32 bytes selection pool
	uint select_a[16];  // indices
	uint select_b[16];  // control words
	uint result[16];
	uint ctrl, sel;

	v128_to_bytes(uvec4(b), sources[0]);
	v128_to_bytes(uvec4(a), sources[1]);
	v128_to_bytes((uvec4(c) & uvec4(0x1F1F1F1F)) ^ (0x1F1F1F1F), select_a); // sel = reverse(sel % 32)
	v128_to_bytes(uvec4(c) & uvec4(0xE0E0E0E0), select_b);                  // ctrl = sel >> 29

	for (int byte = 0; byte < 16; ++byte)
	{
		ctrl = select_b[byte];
		switch (ctrl)
		{
			case 4:        // 100 or 101
			case 5:
				result[byte] = 0;
				break;
			case 6:        // 110
				result[byte] = 0xff;
				break;
			case 7:        // 111
				result[byte] = 0x80;
				break;
			default:
				sel = select_a[byte];
				result[byte] = sources[sel >> 4][sel & 15];
				break;
		}
	}

	return ivec4(v128_from_bytes(result));
}
)"