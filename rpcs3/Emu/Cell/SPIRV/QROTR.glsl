R"(
ivec4 _qrotr(const in ivec4 a, const in int distance)
{
	const int full_iterations = distance >> 5;
	const int rem = distance & 31;
	ivec4 result = a;

	for (int i = 0; i < full_iterations; ++i)
	{
		// Shift right by 1 word
		result = result.wxyz;
	}

	if (rem != 0)
	{
		result = _qrotr32(result, rem);
	}

	return result;
}
)"
