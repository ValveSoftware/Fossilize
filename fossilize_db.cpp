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
#include <memory>
#include <dirent.h>

using namespace std;

namespace Fossilize
{
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

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob) override
	{
		if (mode != DatabaseMode::ReadOnly)
			return false;

		if (!has_entry(tag, hash))
			return false;

		if (!blob_size)
			return false;

		char filename[25]; // 2 digits + "." + 16 digits + ".json" + null
		sprintf(filename, "%02x.%016llx.json", static_cast<unsigned>(tag), static_cast<unsigned long long>(hash));
		auto path = Path::join(base_directory, filename);

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

		if (blob)
		{
			if (*blob_size != file_size)
			{
				fclose(file);
				return false;
			}
		}
		else
			*blob_size = file_size;

		if (blob)
		{
			if (fread(blob, 1, file_size, file) != file_size)
			{
				fclose(file);
				return false;
			}
		}

		fclose(file);
		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size) override
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

		if (fwrite(blob, 1, size, file) != size)
		{
			LOGE("Failed to write serialized state to disk.\n");
			fclose(file);
			return false;
		}

		fclose(file);
		return true;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		size_t size = seen_blobs[tag].size();
		if (hashes)
		{
			if (size != *hash_count)
				return false;
		}
		else
			*hash_count = size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : seen_blobs[tag])
				*iter++ = blob;

			// Make replay more deterministic.
			sort(hashes, hashes + size);
		}
		return true;
	}

	string base_directory;
	DatabaseMode mode;
	unordered_set<Hash> seen_blobs[RESOURCE_COUNT];
};

DatabaseInterface *create_dumb_folder_database(const char *directory_path, DatabaseMode mode)
{
	auto *db = new DumbDirectoryDatabase(directory_path, mode);
	return db;
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

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob) override
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr == end(seen_blobs[tag]))
			return false;

		if (blob)
		{
			if (*blob_size != itr->second.size)
				*blob_size = itr->second.size;
		}
		else
			*blob_size = itr->second.size;

		if (blob)
		{
			if (!mz_zip_reader_extract_to_mem(&mz, itr->second.index, blob, itr->second.size, 0))
			{
				LOGE("Failed to extract blob.\n");
				return false;
			}
		}

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size) override
	{
		if (!alive || mode == DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr != end(seen_blobs[tag]))
			return true;

		char str[32 + 1]; // 32 digits + null
		sprintf(str, "%016x", tag);
		sprintf(str + 16, "%016llx", static_cast<unsigned long long>(hash));
		if (!mz_zip_writer_add_mem(&mz, str, blob, size, MZ_NO_COMPRESSION))
		{
			LOGE("Failed to add blob to cache.\n");
			return false;
		}

		// The index is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{~0u, size});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		size_t size = seen_blobs[tag].size();
		if (hashes)
		{
			if (size != *hash_count)
				return false;
		}
		else
			*hash_count = size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : seen_blobs[tag])
				*iter++ = blob.first;

			// Make replay more deterministic.
			sort(hashes, hashes + size);
		}
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

DatabaseInterface *create_zip_archive_database(const char *path, DatabaseMode mode)
{
	auto *db = new ZipDatabase(path, mode);
	return db;
}

static const uint8_t stream_reference_magic_and_version[16] = {
	0x81, 'F', 'O', 'Z',
	'I', 'L', 'L', 'I',
	'Z', 'E', 'D', 'B',
	0, 0, 0, 3, // 4 bytes to use for versioning.
};

struct StreamArchive : DatabaseInterface
{
	enum { MagicSize = sizeof(stream_reference_magic_and_version) };

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
			// r+b on empty file does not seem to work on Windows, so just fall back to wb.
			if (!file)
				file = fopen(path.c_str(), "wb");
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

				if (memcmp(magic, stream_reference_magic_and_version, MagicSize))
					return false;

				size_t offset = MagicSize;
				size_t begin_append_offset = len;

				while (offset < len)
				{
					begin_append_offset = offset;

					char blob_name[32];
					uint8_t blob_checksum[4];
					uint8_t blob_size[4];

					// Corrupt entry. Our process might have been killed before we could write all data.
					if (offset + sizeof(blob_checksum) + sizeof(blob_name) + sizeof(blob_size) > len)
						break;

					// NAME
					if (fread(blob_name, 1, sizeof(blob_name), file) != sizeof(blob_name))
						return false;
					offset += sizeof(blob_name);

					// CHECKSUM
					if (fread(blob_checksum, 1, sizeof(blob_checksum), file) != sizeof(blob_checksum))
						return false;
					offset += sizeof(blob_checksum);

					// Verify the checksum is using the reserved value of 0 for now.
					uint32_t blob_checksum_le = uint32_t(blob_checksum[0]) |
					                            (uint32_t(blob_checksum[1]) << 8) |
					                            (uint32_t(blob_checksum[2]) << 16) |
					                            (uint32_t(blob_checksum[3]) << 24);
					if (blob_checksum_le != 0)
						break;

					// SIZE
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
				if (fwrite(stream_reference_magic_and_version, 1,
				           sizeof(stream_reference_magic_and_version), file) != sizeof(stream_reference_magic_and_version))
					return false;
			}
		}
		else
		{
			if (fwrite(stream_reference_magic_and_version, 1, sizeof(stream_reference_magic_and_version), file) !=
			    sizeof(stream_reference_magic_and_version))
			{
				return false;
			}
		}

		alive = true;
		return true;
	}

	bool read_entry(ResourceTag tag, Hash hash, size_t *blob_size, void *blob) override
	{
		if (!alive || mode != DatabaseMode::ReadOnly)
			return false;

		auto itr = seen_blobs[tag].find(hash);
		if (itr == end(seen_blobs[tag]))
			return false;

		if (blob)
		{
			if (*blob_size != itr->second.size)
				return false;
		}
		else
			*blob_size = itr->second.size;

		if (blob)
		{
			if (fseek(file, itr->second.offset, SEEK_SET) < 0)
				return false;

			if (fread(blob, 1, itr->second.size, file) != itr->second.size)
				return false;
		}

		return true;
	}

	bool write_entry(ResourceTag tag, Hash hash, const void *blob, size_t size) override
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

		// Reserve 4 bytes for a checksum. For now, just write out 0.
		const char reserved_checksum[4] = {};
		if (fwrite(reserved_checksum, 1, sizeof(reserved_checksum), file) != sizeof(reserved_checksum))
			return false;

		const uint8_t blob_size[4] = {
			uint8_t((size >> 0) & 0xff),
			uint8_t((size >> 8) & 0xff),
			uint8_t((size >> 16) & 0xff),
			uint8_t((size >> 24) & 0xff),
		};

		if (fwrite(blob_size, 1, sizeof(blob_size), file) != sizeof(blob_size))
			return false;

		if (fwrite(blob, 1, size, file) != size)
			return false;

		// The entry is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs[tag].emplace(hash, Entry{0, 0});
		return true;
	}

	bool has_entry(ResourceTag tag, Hash hash) override
	{
		return seen_blobs[tag].count(hash) != 0;
	}

	bool get_hash_list_for_resource_tag(ResourceTag tag, size_t *hash_count, Hash *hashes) override
	{
		size_t size = seen_blobs[tag].size();
		if (hashes)
		{
			if (size != *hash_count)
				return false;
		}
		else
			*hash_count = size;

		if (hashes)
		{
			Hash *iter = hashes;
			for (auto &blob : seen_blobs[tag])
				*iter++ = blob.first;

			// Make replay more deterministic.
			sort(hashes, hashes + size);
		}
		return true;
	}

	struct Entry
	{
		uint64_t offset;
		uint32_t size;
	};

	FILE *file = nullptr;
	string path;
	unordered_map<Hash, Entry> seen_blobs[RESOURCE_COUNT];
	DatabaseMode mode;
	bool alive = false;
};

DatabaseInterface *create_stream_archive_database(const char *path, DatabaseMode mode)
{
	auto *db = new StreamArchive(path, mode);
	return db;
}

DatabaseInterface *create_database(const char *path, DatabaseMode mode)
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
