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

#include <string.h>
#include <atomic>
static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t), "Atomic size mismatch. This type likely requires a lock to work.");

// A simple cross-process FIFO-like mechanism.
// We're not going to bother too much if messages are dropped, since they are mostly informative.

namespace Fossilize
{
enum { ControlBlockMessageSize = 64 };
enum { ControlBlockMagic = 0x19bcde1d };
enum { MaxProcessStats = 256 };

struct SharedControlBlock
{
	uint32_t version_cookie;

	// Used to implement a lock (or spinlock).
	int futex_lock;

	// Progress. Just need atomics to implement this.
	std::atomic<uint32_t> successful_modules;
	std::atomic<uint32_t> successful_graphics;
	std::atomic<uint32_t> successful_compute;
	std::atomic<uint32_t> successful_raytracing;
	std::atomic<uint32_t> skipped_graphics;
	std::atomic<uint32_t> skipped_compute;
	std::atomic<uint32_t> skipped_raytracing;
	std::atomic<uint32_t> cached_graphics;
	std::atomic<uint32_t> cached_compute;
	std::atomic<uint32_t> cached_raytracing;
	std::atomic<uint32_t> clean_process_deaths;
	std::atomic<uint32_t> dirty_process_deaths;
	std::atomic<uint32_t> parsed_graphics;
	std::atomic<uint32_t> parsed_compute;
	std::atomic<uint32_t> parsed_raytracing;
	std::atomic<uint32_t> parsed_graphics_failures;
	std::atomic<uint32_t> parsed_compute_failures;
	std::atomic<uint32_t> parsed_raytracing_failures;
	std::atomic<uint32_t> parsed_module_failures;
	std::atomic<uint32_t> total_graphics;
	std::atomic<uint32_t> total_compute;
	std::atomic<uint32_t> total_raytracing;
	std::atomic<uint32_t> total_modules;
	std::atomic<uint32_t> banned_modules;
	std::atomic<uint32_t> module_validation_failures;
	std::atomic<uint32_t> progress_started;
	std::atomic<uint32_t> progress_complete;

	std::atomic<uint32_t> static_total_count_graphics;
	std::atomic<uint32_t> static_total_count_compute;
	std::atomic<uint32_t> static_total_count_raytracing;

	std::atomic<uint32_t> num_running_processes;
	std::atomic<uint32_t> num_processes_memory_stats;
	std::atomic<uint32_t> metadata_shared_size_mib;
	// Could be 64-bit, but we can express up to ~4 * 10^15 bytes this way.
	// 64-bit would need some form of locking on 32-bit arch.
	std::atomic<uint32_t> process_reserved_memory_mib[MaxProcessStats];
	std::atomic<uint32_t> process_shared_memory_mib[MaxProcessStats];
	std::atomic<int32_t> process_heartbeats[MaxProcessStats];

	std::atomic<int32_t> dirty_pages_mib;
	std::atomic<int32_t> io_stall_percentage;

	// Ring buffer. Needs lock.
	uint32_t write_count;
	uint32_t read_count;
	uint32_t read_offset;
	uint32_t write_offset;
	uint32_t ring_buffer_offset;
	uint32_t ring_buffer_size;
};

// These are not thread-safe. Need to lock them by external means.
static inline uint32_t shared_control_block_read_avail(SharedControlBlock *control_block)
{
	uint32_t ret = control_block->write_count - control_block->read_count;
	return ret;
}

static inline uint32_t shared_control_block_write_avail(SharedControlBlock *control_block)
{
	uint32_t ret = 0;
	uint32_t max_capacity_write_count = control_block->read_count + control_block->ring_buffer_size;
	if (control_block->write_count >= max_capacity_write_count)
		ret = 0;
	else
		ret = max_capacity_write_count - control_block->write_count;
	return ret;
}

static inline bool shared_control_block_read(SharedControlBlock *control_block,
                                             void *data_, uint32_t size)
{
	auto *data = static_cast<uint8_t *>(data_);
	const uint8_t *ring = reinterpret_cast<const uint8_t *>(control_block) + control_block->ring_buffer_offset;

	if (size > control_block->ring_buffer_size)
		return false;

	if (size > (control_block->write_count - control_block->read_count))
		return false;

	uint32_t read_first = control_block->ring_buffer_size - control_block->read_offset;
	uint32_t read_second = 0;
	if (read_first > size)
		read_first = size;
	read_second = size - read_first;

	memcpy(data, ring + control_block->read_offset, read_first);
	if (read_second)
		memcpy(data + read_first, ring, read_second);

	control_block->read_offset = (control_block->read_offset + size) & (control_block->ring_buffer_size - 1);
	control_block->read_count += size;
	return true;
}

static inline bool shared_control_block_write(SharedControlBlock *control_block,
                                              const void *data_, uint32_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);
	uint8_t *ring = reinterpret_cast<uint8_t *>(control_block) + control_block->ring_buffer_offset;

	if (size > control_block->ring_buffer_size)
		return false;

	uint32_t max_capacity_write_count = control_block->read_count + control_block->ring_buffer_size;
	if (control_block->write_count + size > max_capacity_write_count)
		return false;

	uint32_t write_first = control_block->ring_buffer_size - control_block->write_offset;
	uint32_t write_second = 0;
	if (write_first > size)
		write_first = size;
	write_second = size - write_first;

	memcpy(ring + control_block->write_offset, data, write_first);
	if (write_second)
		memcpy(ring, data + write_first, write_second);

	control_block->write_offset = (control_block->write_offset + size) & (control_block->ring_buffer_size - 1);
	control_block->write_count += size;

	return true;
}
}
