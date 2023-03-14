R"(
ivec4 _qrotr32(const in ivec4 src, const in int distance)
{
	const ivec4 tmp = src >> ivec4(distance);
	const ivec4 tmp2 = src.wxyz << ivec4(32 - distance);
	return tmp | tmp2;
}
)"