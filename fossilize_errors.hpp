/* Copyright (c) 2018-2019 Hans-Kristian Arntzen
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

#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <stdint.h>

#if defined(_MSC_VER) && (_MSC_VER <= 1800)
#define FOSSILIZE_NOEXCEPT
#else
#define FOSSILIZE_NOEXCEPT noexcept
#endif

namespace Fossilize
{
static inline void log_error_pnext_chain(std::string what, const void *pNext)
{
	what += " (pNext->sType chain: [";
	while (pNext != nullptr)
	{
		auto *next = static_cast<const VkBaseInStructure *>(pNext);
		what += std::to_string(next->sType);
		pNext = next->pNext;
		if (pNext != nullptr)
			what += ", ";
	}
	what += "])";
	LOGE("Error: %s\n", what.c_str());
}

static inline std::string uint64_string(uint64_t value)
{
	char str[17]; // 16 digits + null
	sprintf(str, "%016llx", static_cast<unsigned long long>(value));
	return str;
}

static inline void log_missing_resource(const char *type, Hash hash)
{
	std::string buffer;
	buffer += "Referenced ";
	buffer += type;
	buffer += " ";
	buffer += uint64_string(hash);
	buffer += ", but it does not exist.";
	LOGE("Error: %s\n", buffer.data());
}
}

