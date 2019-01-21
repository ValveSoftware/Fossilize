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

#include "fossilize_db.hpp"
#include "path.hpp"
#include "layer/utils.hpp"
#include "miniz.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <dirent.h>

using namespace std;

namespace Fossilize
{
// VALVE: Modified to not use std::filesystem
static bool load_buffer_from_path(const std::string &path, vector<uint8_t> &file_data)
{
	FILE *file = fopen(path.c_str(), "rb");
	if (!file)
	{
		LOGE("Failed to open file: %s\n", path.c_str());
		return false;
	}

	if (fseek(file, 0, SEEK_END) < 0)
	{
		fclose(file);
		LOGE("Failed to seek in file: %s\n", path.c_str());
		return false;
	}

	size_t file_size = size_t(ftell(file));
	rewind(file);

	file_data.resize(file_size);
	if (fread(file_data.data(), 1, file_size, file) != file_size)
	{
		LOGE("Failed to read from file: %s\n", path.c_str());
		fclose(file);
		return false;
	}

	fclose(file);
	return true;
}

struct DumbDirectoryDatabase : DatabaseInterface
{
	DumbDirectoryDatabase(const string &base, DatabaseMode mode_)
		: base_directory(base), mode(mode_)
	{
	}

	bool prepare() override
	{
		if (mode == DatabaseMode::OverWrite)
			return true;

		DIR *dp = opendir(base_directory.c_str());
		if (!dp)
			return false;

		while (auto *pEntry = readdir(dp))
		{
			if (pEntry->d_type != DT_REG)
				continue;

			unsigned tag;
			unsigned long long value;
			if (sscanf(pEntry->d_name, "%x.%llx.json", &tag, &value) != 2)
				continue;

			if (tag >= RESOURCE_COUNT)
				continue;

			seen_blobs[tag].insert(value);
		}

		closedir(dp);
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		return seen_blobs[tag].count(hash) != 0;
	}

	bool read_entry(ResourceTag tag, Hash hash, vector<uint8_t> &blob) override
	{
		if (mode != DatabaseMode::ReadOnly)
			return false;

		if (!has_entry(tag, hash))
			return false;

		char filename[25]; // 2 digits + "." + 16 digits + ".json" + null
		sprintf(filename, "%02x.%016llx.json", static_cast<unsigned>(tag), static_cast<unsigned long long>(hash));
		auto path = Path::join(base_directory, filename);
		return load_buffer_from_path(path, blob);
	}

	bool write_entry(ResourceTag tag, Hash hash, const std::vector<uint8_t> &bytes) override
	{
		if (mode == DatabaseMode::ReadOnly)
			return false;

		if (has_entry(tag, hash))
			return true;

		char filename[25]; // 2 digits + "." + 16 digits + ".json" + null
		sprintf(filename, "%02x.%016llx.json", static_cast<unsigned>(tag), static_cast<unsigned long long>(hash));
		auto path = Path::join(base_directory, filename);

		FILE *file = fopen(path.c_str(), "wb");
		if (!file)
		{
			LOGE("Failed to write serialized state to disk (%s).\n", path.c_str());
			return false;
		}

		if (fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
		{
			LOGE("Failed to write serialized state to disk.\n");
			fclose(file);
			return false;
		}

		fclose(file);
		return true;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, vector<Hash> &hashes) override
	{
		hashes.clear();
		hashes.reserve(seen_blobs[tag].size());
		for (auto &blob : seen_blobs[tag])
			hashes.push_back(blob);

		// Make replay more deterministic.
		sort(begin(hashes), end(hashes));
		return true;
	}

	string base_directory;
	DatabaseMode mode;
	unordered_set<Hash> seen_blobs[RESOURCE_COUNT];
};

unique_ptr<DatabaseInterface> create_dumb_folder_database(const string &directory_path, DatabaseMode mode)
{
	auto db = make_unique<DumbDirectoryDatabase>(directory_path, mode);
	return move(db);
}

struct ZipDatabase : DatabaseInterface
{
	ZipDatabase(const string &path_, DatabaseMode mode_)
		: path(path_), mode(mode_)
	{
	}

	~ZipDatabase()
	{
		if (alive)
		{
			if (mode != DatabaseMode::ReadOnly)
			{
				if (!mz_zip_writer_finalize_archive(&mz))
					LOGE("Failed to finalize archive.\n");
			}

			if (!mz_zip_end(&mz))
				LOGE("mz_zip_end failed!\n");
		}
	}

	static bool string_is_hex(const char *str)
	{
		while (*str)
		{
			if (!isxdigit(*str))
				return false;
			str++;
		}
		return true;
	}

	bool prepare() override
	{
		mz_zip_zero_struct(&mz);

		if (mode != DatabaseMode::OverWrite && mz_zip_reader_init_file(&mz, path.c_str(), 0))
		{
			// We have an existing archive.
			unsigned files = mz_zip_reader_get_num_files(&mz);
			char filename[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE] = {};

			for (unsigned i = 0; i < files; i++)
			{
				if (mz_zip_reader_is_file_a_directory(&mz, i))
					continue;

				mz_zip_reader_get_filename(&mz, i, filename, sizeof(filename));
				size_t len = strlen(filename);
				if (len != 32)
					continue;

				if (!string_is_hex(filename))
					continue;

				mz_zip_archive_file_stat s;
				if (!mz_zip_reader_file_stat(&mz, i, &s))
					continue;

				char tag_str[16 + 1] = {};
				char value_str[16 + 1] = {};
				memcpy(tag_str, filename, 16);
				memcpy(value_str, filename + 16, 16);

				unsigned tag = unsigned(strtoul(tag_str, nullptr, 16));
				if (tag >= RESOURCE_COUNT)
					continue;
				uint64_t value = strtoull(value_str, nullptr, 16);
				seen_blobs[tag].emplace(value, Entry{i, size_t(s.m_uncomp_size)});
			}

			// In-place update the archive. Should we consider emitting a new archive instead?
			if (!mz_zip_writer_init_from_reader(&mz, path.c_str()))
			{
				LOGE("Failed to initialize ZIP writer from reader.\n");
				mz_zip_end(&mz);
				return false;
			}

			alive = true;
		}
		else if (mode != DatabaseMode::ReadOnly)
		{
			if (!mz_zip_writer_init_file(&mz, path.c_str(), 0))
			{
				LOGE("Failed to open ZIP archive for writing. Cannot serialize anything to disk.\n");
				return false;
			}

			alive = true;

			for (auto &blob : seen_blobs)
				blob.clear();
		}

		return true;
	}

	bool read_entry(ResourceTag tag, Hash hash, vector<uint8_t> &blob) override
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr == end(seen_blobs[tag]))
			return false;

		blob.resize(itr->second.size);
		if (!mz_zip_reader_extract_to_mem(&mz, itr->second.index, blob.data(), blob.size(), 0))
		{
			LOGE("Failed to extract blob.\n");
			return false;
		}

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const vector<uint8_t> &blob) override
	{
		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[32 + 1]; // 32 digits + null
		sprintf(str, "%016x", tag);
		sprintf(str + 16, "%016llx", static_cast<unsigned long long>(hash));
		if (!mz_zip_writer_add_mem(&mz, str, blob.data(), blob.size(), MZ_NO_COMPRESSION))
		{
			LOGE("Failed to add blob to cache.\n");
			return false;
		}

		// The index is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{~0u, blob.size()});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, vector<Hash> &hashes) override
	{
		hashes.clear();
		hashes.reserve(seen_blobs[tag].size());
		for (auto &blob : seen_blobs[tag])
			hashes.push_back(blob.first);

		// Make replay more deterministic.
		sort(begin(hashes), end(hashes));
		return true;
	}

	string path;
	mz_zip_archive mz;

	struct Entry
	{
		unsigned index;
		size_t size;
	};

	unordered_map<Hash, Entry> seen_blobs[RESOURCE_COUNT];
	DatabaseMode mode;
	bool alive = false;
};

unique_ptr<DatabaseInterface> create_zip_archive_database(const string &path, DatabaseMode mode)
{
	auto db = make_unique<ZipDatabase>(path, mode);
	return move(db);
}

static const uint8_t stream_reference_magic[] = {
	0x81, 'S', 'T', 'R', 'E', 'A', 'M', 0,
};

struct StreamArchive : DatabaseInterface
{
	enum { MagicSize = 8 };

	StreamArchive(const string &path_, DatabaseMode mode_)
		: path(path_), mode(mode_)
	{
	}

	~StreamArchive()
	{
		if (file)
			fclose(file);
	}

	bool prepare() override
	{
		switch (mode)
		{
		case DatabaseMode::ReadOnly:
			file = fopen(path.c_str(), "rb");
			break;

		case DatabaseMode::Append:
			file = fopen(path.c_str(), "r+b");
			break;

		case DatabaseMode::OverWrite:
			file = fopen(path.c_str(), "wb");
			break;
		}

		if (!file)
			return false;

		if (mode != DatabaseMode::OverWrite)
		{
			// Scan through the archive and get the list of files.
			fseek(file, 0, SEEK_END);
			size_t len = ftell(file);
			rewind(file);

			if (len != 0)
			{
				uint8_t magic[MagicSize];
				if (fread(magic, 1, MagicSize, file) != MagicSize)
					return false;

				if (memcmp(magic, stream_reference_magic, MagicSize))
					return false;

				size_t offset = MagicSize;
				size_t begin_append_offset = len;

				while (offset < len)
				{
					begin_append_offset = offset;

					char blob_name[32];
					uint8_t blob_size[4];

					// Corrupt entry. Our process might have been killed before we could write all data.
					if (offset + sizeof(blob_name) + sizeof(blob_size) > len)
						break;

					if (fread(blob_name, 1, sizeof(blob_name), file) != sizeof(blob_name))
						return false;
					offset += sizeof(blob_name);

					if (fread(blob_size, 1, sizeof(blob_size), file) != sizeof(blob_size))
						return false;
					offset += sizeof(blob_size);

					uint32_t blob_size_le = uint32_t(blob_size[0]) |
					                        (uint32_t(blob_size[1]) << 8) |
					                        (uint32_t(blob_size[2]) << 16) |
					                        (uint32_t(blob_size[3]) << 24);

					// Corrupt entry. Our process might have been killed before we could write all data.
					if (offset + blob_size_le > len)
						break;

					char tag_str[16 + 1] = {};
					char value_str[16 + 1] = {};
					memcpy(tag_str, blob_name, 16);
					memcpy(value_str, blob_name + 16, 16);

					unsigned tag = unsigned(strtoul(tag_str, nullptr, 16));
					if (tag < RESOURCE_COUNT)
					{
						uint64_t value = strtoull(value_str, nullptr, 16);
						seen_blobs[tag].emplace(value, Entry{offset, blob_size_le});
					}

					if (fseek(file, blob_size_le, SEEK_CUR) < 0)
						return false;

					offset += blob_size_le;
				}

				if (mode == DatabaseMode::Append && offset != len)
				{
					if (fseek(file, begin_append_offset, SEEK_SET) < 0)
						return false;
				}
			}
			else
			{
				// Appending to a fresh file. Make sure we have the magic.
				if (fwrite(stream_reference_magic, 1, sizeof(stream_reference_magic), file) != sizeof(stream_reference_magic))
					return false;
			}
		}
		else
		{
			if (fwrite(stream_reference_magic, 1, sizeof(stream_reference_magic), file) != sizeof(stream_reference_magic))
				return false;
		}

		alive = true;
		return true;
	}

	bool read_entry(ResourceTag tag, Hash hash, vector<uint8_t> &blob)
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr == end(seen_blobs[tag]))
			return false;

		if (fseek(file, itr->second.offset, SEEK_SET) < 0)
			return false;
		blob.resize(itr->second.size);

		if (fread(blob.data(), 1, itr->second.size, file) != itr->second.size)
			return false;

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const vector<uint8_t> &blob)
	{
		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[32 + 1]; // 32 digits + null
		sprintf(str, "%016x", tag);
		sprintf(str + 16, "%016llx", static_cast<unsigned long long>(hash));

		if (fwrite(str, 1, 32, file) != 32)
			return false;

		const uint8_t blob_size[4] = {
			uint8_t((blob.size() >> 0) & 0xff),
			uint8_t((blob.size() >> 8) & 0xff),
			uint8_t((blob.size() >> 16) & 0xff),
			uint8_t((blob.size() >> 24) & 0xff),
		};

		if (fwrite(blob_size, 1, sizeof(blob_size), file) != sizeof(blob_size))
			return false;

		if (fwrite(blob.data(), 1, blob.size(), file) != blob.size())
			return false;

		// The entry is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{0, 0});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, vector<Hash> &hashes) override
	{
		hashes.clear();
		hashes.reserve(seen_blobs[tag].size());
		for (auto &blob : seen_blobs[tag])
			hashes.push_back(blob.first);

		// Make replay more deterministic.
		sort(begin(hashes), end(hashes));
		return true;
	}

	struct Entry
	{
		uint64_t offset;
		uint32_t size;
	};

	FILE *file;
	string path;
	unordered_map<Hash, Entry> seen_blobs[RESOURCE_COUNT];
	DatabaseMode mode;
	bool alive = false;
};

unique_ptr<DatabaseInterface> create_stream_archive_database(const string &path, DatabaseMode mode)
{
	auto db = make_unique<StreamArchive>(path, mode);
	return move(db);
}

unique_ptr<DatabaseInterface> create_database(const string &path, DatabaseMode mode)
{
	auto ext = Path::ext(path);
	if (ext == "foz")
		return create_stream_archive_database(path, mode);
	else if (ext == "zip")
		return create_zip_archive_database(path, mode);
	else
		return create_dumb_folder_database(path, mode);
}

}
