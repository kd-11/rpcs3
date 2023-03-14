R"(
ivec4 _qshl(const in ivec4 a, const in int distance)
{
	const ivec4 result = _qrotl(a, distance);
	const ivec4 mask = qshl_mask_lookup[distance];
	return result & mask;
}
)"
