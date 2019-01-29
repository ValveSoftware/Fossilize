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

#include "fossilize.hpp"
#include <stdint.h>
#include <stddef.h>

namespace Fossilize
{
enum ResourceTag
{
	RESOURCE_APPLICATION_INFO = 0,
	RESOURCE_SAMPLER = 1,
	RESOURCE_DESCRIPTOR_SET_LAYOUT = 2,
	RESOURCE_PIPELINE_LAYOUT = 3,
	RESOURCE_SHADER_MODULE = 4,
	RESOURCE_RENDER_PASS = 5,
	RESOURCE_GRAPHICS_PIPELINE = 6,
	RESOURCE_COMPUTE_PIPELINE = 7,
	RESOURCE_COUNT = 8
};

enum PayloadWriteFlagBits
{
	PAYLOAD_WRITE_NO_FLAGS = 0,

	// Can only be used for the stream_archive_database.
	// If used, the blob data is considered opaque and may be compressed in some unspecified scheme.
	// The only use for this flag is to transparently transfer payloads to other stream archive databases.
	PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT = 1 << 0,

	// If applicable to the backend, compresses the payload.
	PAYLOAD_WRITE_COMPRESS_BIT = 1 << 1,

	// If WRITE_COMPRESS_BIT is set, prefer slower compression algorithms.
	PAYLOAD_WRITE_BEST_COMPRESSION_BIT = 1 << 2,

	// Compute checksum of payload for more robustness.
	PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT = 1 << 3,

	PAYLOAD_WRITE_MAX_ENUM = 0x7fffffff
};

enum PayloadReadFlagBits
{
	PAYLOAD_READ_NO_FLAGS = 0,

	// Can only be used for the stream_archive_database.
	// If used, the blob data is considered opaque and may be compressed in some unspecified scheme.
	// The only use for this flag is to transparently transfer payloads to other stream archive databases.
	PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT = 1 << 0,

	PAYLOAD_READ_MAX_ENUM = 0x7fffffff
};
using PayloadWriteFlags = uint32_t;
using PayloadReadFlags = uint32_t;

// This is an interface to interact with an external database for blob modules.
// It is is a simple database with key + blob.
// NOTE: The database is NOT thread-safe.
class DatabaseInterface
{
public:
	virtual ~DatabaseInterface() = default;

	// Prepares the database. It can load in the off-line archive from disk.
	virtual bool prepare() = 0;

	// Reads a blob entry from database.
	// Arguments are similar to Vulkan, call the query function twice.
	// First, call with buffer == nullptr to query size.
	// Then, pass in allocated buffer, *size must match the previously queried size.
	// The same flags must be passed when just querying size and reading data into buffer.
	virtual bool read_entry(ResourceTag tag, Hash hash, size_t *size, void *buffer, PayloadReadFlags flags) = 0;

	// Writes an entry to database.
	virtual bool write_entry(ResourceTag tag, Hash hash, const void *buffer, size_t size, PayloadWriteFlags flags) = 0;

	// Checks if entry already exists in database, i.e. no need to serialize.
	virtual bool has_entry(ResourceTag tag, Hash hash) = 0;

	// Arguments are similar to Vulkan, call the query function twice.
	virtual bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *num_hashes, Hash *hash) = 0;
};

enum class DatabaseMode
{
	Append,
	ReadOnly,
	OverWrite,
	ExclusiveOverWrite
};

DatabaseInterface *create_dumb_folder_database(const char *directory_path, DatabaseMode mode);
DatabaseInterface *create_zip_archive_database(const char *path, DatabaseMode mode);
DatabaseInterface *create_stream_archive_database(const char *path, DatabaseMode mode);
DatabaseInterface *create_database(const char *path, DatabaseMode mode);

// This is a special kind of database which can be used from multiple threads and splits out the database
// into a read-only part and a write-only part, which is unique for each instance of this database.
// base_path.foz is the read-only database. If it does not exist, it will not be written to either.
// If there are any writes to the database which do not already exist in the read-only database, a new
// database will be created at base_path.%d.foz, where %d is a unique index. Exclusive file open mechanisms are used
// to ensure correctness.
DatabaseInterface *create_concurrent_database(const char *base_path);

// Merges stream archives found in source_paths into append_database_path.
bool merge_concurrent_databases(const char *append_database_path, const char * const *source_paths, size_t num_source_paths);
}