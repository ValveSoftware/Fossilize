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

#include "varint.hpp"

namespace Fossilize
{
size_t compute_size_varint(const uint32_t *words, size_t word_count)
{
	size_t size = 0;
	for (size_t i = 0; i < word_count; i++)
	{
		auto w = words[i];
		if (w < (1u << 7))
			size += 1;
		else if (w < (1u << 14))
			size += 2;
		else if (w < (1u << 21))
			size += 3;
		else if (w < (1u << 28))
			size += 4;
		else
			size += 5;
	}
	return size;
}

uint8_t *encode_varint(uint8_t *buffer, const uint32_t *words, size_t word_count)
{
	for (size_t i = 0; i < word_count; i++)
	{
		auto w = words[i];
		if (w < (1u << 7))
			*buffer++ = uint8_t(w);
		else if (w < (1u << 14))
		{
			*buffer++ = uint8_t(0x80u | ((w >> 0) & 0x7f));
			*buffer++ = uint8_t((w >> 7) & 0x7f);
		}
		else if (w < (1u << 21))
		{
			*buffer++ = uint8_t(0x80u | ((w >> 0) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 7) & 0x7f));
			*buffer++ = uint8_t((w >> 14) & 0x7f);
		}
		else if (w < (1u << 28))
		{
			*buffer++ = uint8_t(0x80u | ((w >> 0) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 7) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 14) & 0x7f));
			*buffer++ = uint8_t((w >> 21) & 0x7f);
		}
		else
		{
			*buffer++ = uint8_t(0x80u | ((w >> 0) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 7) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 14) & 0x7f));
			*buffer++ = uint8_t(0x80u | ((w >> 21) & 0x7f));
			*buffer++ = uint8_t((w >> 28) & 0x7f);
		}
	}
	return buffer;
}

bool decode_varint(uint32_t *words, size_t words_size, const uint8_t *buffer, size_t buffer_size)
{
	size_t offset = 0;
	for (size_t i = 0; i < words_size; i++)
	{
		auto &w = words[i];
		w = 0;

		uint32_t shift = 0;
		do
		{
			if (offset >= buffer_size || shift >= 32u)
				return false;

			w |= (buffer[offset] & 0x7f) << shift;
			shift += 7;
		} while (buffer[offset++] & 0x80);
	}

	return buffer_size == offset;
}
}
