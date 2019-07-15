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

#include "util/object_cache.hpp"
#include "layer/utils.hpp"
#include <stdlib.h>

using namespace Fossilize;

int main()
{
	ObjectCache<int> cache;
	cache.set_target_size(0);

	// Trivial test, insert two objects and delete the cache.
	cache.insert_object(1, 1000, 10000);
	cache.insert_object(2, 2000, 20000);
	if (cache.get_current_total_size() != 30000)
		abort();

	cache.delete_cache([](Hash, int object) {
		LOGI("Deleting object: %d\n", object);
	});

	if (cache.get_current_total_size() != 0)
		abort();
	if (cache.get_current_object_count() != 0)
		abort();

	// Try inserting lots of objects. Access objects with size 3 and 17.
	// After pruning, only those two objects should remain.
	cache.set_target_size(20);
	for (unsigned i = 0; i < 10000; i++)
		cache.insert_object(i, i * 1000, i);
	if (cache.find_object(9999).first != 9999000)
		abort();
	if (cache.find_object(3).first != 3000)
		abort();
	if (cache.find_object(17).first != 17000)
		abort();

	cache.prune_cache([](Hash, int) {});

	if (cache.get_current_total_size() != 20)
		abort();
	if (cache.get_current_object_count() != 2)
		abort();

	if (cache.find_object(3).first == 0)
		abort();
	if (cache.find_object(17).first == 0)
		abort();
	if (cache.find_object(9999).first != 0)
		abort();

	cache.delete_cache([](Hash, int object) {
		LOGI("Deleting object: %d\n", object);
	});

	if (cache.get_current_total_size() != 0)
		abort();
	if (cache.get_current_object_count() != 0)
		abort();
}
