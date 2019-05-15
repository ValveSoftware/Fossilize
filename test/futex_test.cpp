/* Copyright (c) 2019 Hans-Kristian Arntzen
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

#ifdef __linux__
#include "platform/futex_wrapper_linux.hpp"
#else
#include "platform/gcc_clang_spinlock.hpp"
#endif

#include <pthread.h>
#include <stdlib.h>

static unsigned global_variable;

static const unsigned num_threads = 64;
static const unsigned num_iterations = 100000;

static int lock;

static void *looper(void *)
{
	for (unsigned i = 0; i < num_iterations; i++)
	{
		Fossilize::futex_wrapper_lock(&lock);
		global_variable++;
		Fossilize::futex_wrapper_unlock(&lock);
	}
	return nullptr;
}

int main()
{
	pthread_t threads[num_threads];
	for (auto &t : threads)
		pthread_create(&t, nullptr, looper, nullptr);

	for (auto &t : threads)
		pthread_join(t, nullptr);

	if (global_variable != num_iterations * num_threads)
		return EXIT_FAILURE;
	else
		return EXIT_SUCCESS;
}