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

#pragma once

#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

// Implementation based on https://eli.thegreenplace.net/2018/basics-of-futexes/ and
// "Futexes are Tricky" by Ulrich Drepper.
// Kind of overkill, but we need a mutex which can work cross-process and cross-architecture via shared memory (32-bit and 64-bit).
// Alternative is full kernel semaphores or raw spinlocks.

namespace Fossilize
{
static inline int cmpxchg(int *value, int expected_value, int new_value)
{
	int ret = __sync_val_compare_and_swap(value, expected_value, new_value);
	return ret;
}

static inline void futex_wrapper_lock(int *lock)
{
	int c = cmpxchg(lock, 0, 1);
	if (c != 0)
	{
		// Contention.
		do
		{
			// Need to lock. Force *lock to be 2.
			if (c == 2 || cmpxchg(lock, 1, 2) != 0)
			{
				// If *lock is 2 (was not unlocked somehow by other thread),
				// wait until it's woken up.
				syscall(SYS_futex, lock, FUTEX_WAIT, 2, 0, 0, 0);
			}
		} while ((c = cmpxchg(lock, 0, 2)) != 0);
	}
}

static inline void futex_wrapper_unlock(int *lock)
{
	int c = __sync_sub_and_fetch(lock, 1);
	if (c == 1)
	{
		// We have some waiters to wake up.

		// Atomic store, really, but there's no __sync variant for that.
		__sync_fetch_and_and(lock, 0);

		syscall(SYS_futex, lock, FUTEX_WAKE, 1, 0, 0, 0);
	}
}
}