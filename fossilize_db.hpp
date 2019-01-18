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

#include <stdint.h>
#include <vector>
#include <string>
#include <memory>
#include "fossilize.hpp"

namespace Fossilize
{
enum ResourceTag
{
	RESOURCE_SAMPLER = 0,
	RESOURCE_DESCRIPTOR_SET_LAYOUT = 1,
	RESOURCE_PIPELINE_LAYOUT = 2,
	RESOURCE_SHADER_MODULE = 3,
	RESOURCE_RENDER_PASS = 4,
	RESOURCE_GRAPHICS_PIPELINE = 5,
	RESOURCE_COMPUTE_PIPELINE = 6,
	RESOURCE_COUNT = 7
};

// This is an interface to interact with an external database for blob modules.
// It is is a simple database with key + blob.
class DatabaseInterface
{
public:
	virtual ~DatabaseInterface() = default;

	// Prepares the database. It can load in the off-line archive from disk.
	virtual bool prepare() = 0;

	// Reads a blob entry from database.
	virtual bool read_entry(ResourceTag tag, Hash hash, std::vector<uint8_t> &blob) = 0;

	// Writes an entry to database.
	virtual bool write_entry(ResourceTag tag, Hash hash, const std::vector<uint8_t> &blob) = 0;

	// Checks if entry already exists in database, i.e. no need to serialize.
	virtual bool has_entry(ResourceTag tag, Hash hash) = 0;

	virtual bool get_hash_list_for_resource_tag(ResourceTag tag, std::vector<Hash> &hashes) = 0;
};

enum class DatabaseMode
{
	Append,
	ReadOnly,
	OverWrite
};

std::unique_ptr<DatabaseInterface> create_dumb_folder_database(const std::string &directory_path, DatabaseMode mode);
std::unique_ptr<DatabaseInterface> create_zip_archive_database(const std::string &path, DatabaseMode mode);
}