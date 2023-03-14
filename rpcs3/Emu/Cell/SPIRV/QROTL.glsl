R"(
ivec4 _qrotl(const in ivec4 a, const in int distance)
{
	const int full_iterations = distance >> 5;
	const int rem = distance & 31;
	ivec4 result = a;

	for (int i = 0; i < full_iterations; ++i)
	{
		// Shift left by 1 word
		result = result.yzwx;
	}

	if (rem != 0)
	{
		result = _qrotl32(result, rem);
	}

	return result;
}
)"
