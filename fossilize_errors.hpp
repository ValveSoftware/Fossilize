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

#include <string>
#include <utility>
#include <stdint.h>
#include "fossilize_inttypes.h"

#if defined(_MSC_VER) && (_MSC_VER <= 1800)
#define FOSSILIZE_NOEXCEPT
#else
#define FOSSILIZE_NOEXCEPT noexcept
#endif

namespace Fossilize
{
// Sets the global logging level for the current thread.
// Any internal threads created by Fossilize will inherit the log level from creating thread.
// FOSSILIZE_API_DEFAULT_LOG_LEVEL define can be used to set initial value.
void set_thread_log_level(LogLevel level);
LogLevel get_thread_log_level();
#define LOGE_LEVEL(...) do { if (get_thread_log_level() <= LOG_ERROR) { LOGE(__VA_ARGS__); } } while(0)
#define LOGW_LEVEL(...) do { if (get_thread_log_level() < LOG_ERROR) { LOGW(__VA_ARGS__); } } while(0)

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
	LOGW_LEVEL("%s\n", what.c_str());
}

static inline void log_missing_resource(const char *type, Hash hash)
{
	LOGW_LEVEL("Referenced %s %016" PRIx64
			           ", but it does not exist.\n"
			           "This can be expected when replaying an archive from Steam.\n"
			           "If replaying just the application cache, "
			           "make sure to replay together with the common cache.\n",
	           type, hash);
}

static inline void log_invalid_resource(const char *type, Hash hash)
{
	LOGW_LEVEL("Referenced %s %016" PRIx64
			           ", but it is VK_NULL_HANDLE.\n"
			           "The create info was likely not supported by device.\n",
	           type, hash);
}

template <typename T>
static inline void log_failed_hash(const char *type, T object)
{
	LOGW_LEVEL("%s handle 0x%016" PRIx64
			           " is not registered.\n"
			           "It has either not been recorded, or it failed to be recorded earlier "
			           "(which is expected if application uses an extension that is not recognized by Fossilize).\n",
	           type, (uint64_t)object);
}
}

