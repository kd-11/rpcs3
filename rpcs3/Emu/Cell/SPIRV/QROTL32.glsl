R"(
ivec4 _qrotl32(const in ivec4 src, const in int distance)
{
	const ivec4 tmp = src << ivec4(distance);
	const ivec4 tmp2 = src.yzwx >> ivec4(32 - distance);
	return tmp | tmp2;
}
)"