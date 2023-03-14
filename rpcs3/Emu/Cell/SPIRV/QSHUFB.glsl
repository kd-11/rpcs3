R"(
ivec4 _dqshufb(const in ivec4 a, const in ivec4 b, const in ivec4 c)
{
	const uvec4 sources[2] = { uvec4(b), uvec4(a) };
	uint tmp, tmp2, tmp3, ref;
	uvec4 result;

	for (int word = 0; word < 4; ++word)
	{
		for (int bit = 0; bit < 32; bit += 8)
		{
			ref = bitfieldExtract(uint(c[word]), bit, 8);     // ref = shuffle word
			tmp = bitfieldExtract(ref, 29, 3);          // tmp = control word
			tmp2 = ref & 15;                            // tmp2 = ref % 16

			switch (tmp)
			{
				case 4:       // 100 or 101
				case 5:
				{
					tmp3 = 0;
					break;
				}
				case 6:       // 110
				{
					tmp3 = 0xFF;
					break;
				}
				case 7:       // 111
				{
					tmp3 = 0x80;
					break;
				}
				default:
				{
					tmp3 = bitfieldExtract(sources[(ref >> 4) & 1][tmp2 >> 2], int(tmp2 & 3) << 3, 8);
					break;
				}
			}

			bitfieldInsert(result[word], tmp3, bit, 8);
		}
	}

	return ivec4(result);
}
)"