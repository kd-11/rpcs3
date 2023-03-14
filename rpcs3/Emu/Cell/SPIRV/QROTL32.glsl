R"(
ivec4 _qrotl32(const in ivec4 src, const in int distance)
{
	const ivec4 tmp = src << ivec4(distance);
	const ivec4 tmp2 = bitfieldExtract(src.yzwx, 32 - distance, distance);
	return tmp | tmp2;
}
)"