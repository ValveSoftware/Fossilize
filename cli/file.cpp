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

#include "file.hpp"
#include <stdio.h>

namespace Fossilize
{
std::vector<uint8_t> load_buffer_from_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return {};

	fseek(file, 0, SEEK_END);
	long len = ftell(file);
	rewind(file);
	std::vector<uint8_t> ret(len);
	if (fread(ret.data(), 1, len, file) != size_t(len))
	{
		fclose(file);
		return {};
	}

	fclose(file);
	return ret;
}

bool write_string_to_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	if (!file)
		return false;

	if (fputs(text, file) == EOF)
	{
		fclose(file);
		return false;
	}

	fclose(file);
	return true;
}

bool write_buffer_to_file(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	if (!file)
		return false;

	if (fwrite(data, 1, size, file) != size)
	{
		fclose(file);
		return false;
	}

	fclose(file);
	return true;
}
}
