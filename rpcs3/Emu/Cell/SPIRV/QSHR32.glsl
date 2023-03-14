R"(
ivec4 _qshr32(const in ivec4 a, const in int distance)
{
	const ivec4 result = _qrotr32(a, distance);
	const ivec4 mask = qshr_mask_lookup[distance];
	return result & mask;
}
)"