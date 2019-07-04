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

#include <unordered_map>
#include "fossilize_types.hpp"
#include "object_pool.hpp"
#include "intrusive_list.hpp"
#include <assert.h>

namespace Fossilize
{
template <typename T>
class ObjectCache
{
public:
	ObjectCache() = default;
	ObjectCache(const ObjectCache &) = delete;
	void operator=(const ObjectCache &) = delete;

	~ObjectCache()
	{
		assert(lru_cache.empty());
	}

	void set_target_size(size_t size)
	{
		target_size = size;
	}

	std::pair<T, bool> find_object(Hash hash)
	{
		auto itr = hash_to_objects.find(hash);
		if (itr == std::end(hash_to_objects))
			return { static_cast<T>(0), false };

		lru_cache.move_to_front(lru_cache, itr->second);
		return { itr->second->object, true };
	}

	template <typename Deleter>
	void prune_cache(const Deleter &deleter)
	{
		while (total_size > target_size)
		{
			assert(!lru_cache.empty());
			auto last_used_entry = lru_cache.rbegin();
			assert(last_used_entry->size <= total_size);
			total_size -= last_used_entry->size;
			lru_cache.erase(last_used_entry);

			deleter(last_used_entry->hash, last_used_entry->object);
			hash_to_objects.erase(last_used_entry->hash);
			pool.free(last_used_entry.get());
		}
	}

	template <typename Deleter>
	void delete_cache(const Deleter &deleter)
	{
		auto itr = lru_cache.begin();
		while (itr != std::end(lru_cache))
		{
			auto entry = itr;
			itr = lru_cache.erase(entry);
			deleter(entry->hash, entry->object);
			assert(entry->size <= total_size);
			total_size -= entry->size;
			pool.free(entry.get());
		}

		lru_cache.clear();
		hash_to_objects.clear();
		assert(total_size == 0);
	}

	void insert_object(Hash hash, T object, size_t object_size)
	{
		auto *entry = pool.allocate();
		entry->hash = hash;
		entry->object = object;
		entry->size = object_size;
		lru_cache.insert_front(entry);
		auto map_itr = hash_to_objects.insert({ hash, entry });
		(void)map_itr;
		assert(map_itr.second);

		total_size += object_size;
	}

	size_t get_current_total_size() const
	{
		return total_size;
	}

	size_t get_current_object_count() const
	{
		return hash_to_objects.size();
	}

private:
	size_t target_size = 0;
	size_t total_size = 0;

	struct CacheEntry : IntrusiveListEnabled<CacheEntry>
	{
		T object = static_cast<T>(0);
		Hash hash = 0;
		size_t size = 0;
	};

	ObjectPool<CacheEntry> pool;
	std::unordered_map<Hash, CacheEntry *> hash_to_objects;
	IntrusiveList<CacheEntry> lru_cache;
};
}
