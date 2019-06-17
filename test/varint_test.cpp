/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "fossilize.hpp"
#include "varint.hpp"
#include <string.h>
#include <random>
#include <vector>

using namespace Fossilize;

int main()
{
	std::mt19937 rnd;
	std::vector<uint32_t> buffer;
	buffer.reserve(16 * 1024 * 1024);
	for (unsigned i = 0; i < 16 * 1024 * 1024; i++)
		buffer.push_back(uint32_t(rnd()) & ((1u << 29) - 1));

	size_t computed = compute_size_varint(buffer.data(), buffer.size());
	std::vector<uint8_t> encode_buffer(computed);
	encode_varint(encode_buffer.data(), buffer.data(), buffer.size());

	std::vector<uint32_t> decode_buffer(16 * 1024 * 1024);
	if (!decode_varint(decode_buffer.data(), decode_buffer.size(), encode_buffer.data(), encode_buffer.size()))
		return EXIT_FAILURE;

	if (memcmp(buffer.data(), decode_buffer.data(), decode_buffer.size() * sizeof(uint32_t)))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
