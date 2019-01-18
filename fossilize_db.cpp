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
#include <inttypes.h>
#include "miniz.h"
#include <unordered_map>
#include <miniz/miniz.h>

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
	void set_base_directory(const string &base)
	{
		base_directory = base;
	}

	void prepare() override
	{
	}

	bool has_entry(Hash) override
	{
		return false;
	}

	bool read_entry(Hash hash, vector<uint8_t> &blob) override
	{
		char filename[22];
		sprintf(filename, "%016" PRIX64 ".json", hash);
		auto path = Path::join(base_directory, filename);
		return load_buffer_from_path(path, blob);
	}

	bool write_entry(Hash hash, const std::vector<uint8_t> &bytes) override
	{
		char filename[22]; // 16 digits + ".json" + null
		sprintf(filename, "%016" PRIX64 ".json", hash);
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

	string base_directory;
};

unique_ptr<DatabaseInterface> create_dumb_folder_database(const string &directory_path)
{
	auto db = make_unique<DumbDirectoryDatabase>();
	db->set_base_directory(directory_path);
	return move(db);
}

struct ZipDatabase : DatabaseInterface
{
	ZipDatabase(const string &path_, bool readonly_)
		: path(path_), readonly(readonly_)
	{
	}

	~ZipDatabase()
	{
		if (alive)
		{
			if (!readonly)
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

	void prepare() override
	{
		mz_zip_zero_struct(&mz);

		if (mz_zip_reader_init_file(&mz, path.c_str(), 0))
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
				if (len != 16)
					continue;

				if (!string_is_hex(filename))
					continue;

				mz_zip_archive_file_stat s;
				if (!mz_zip_reader_file_stat(&mz, i, &s))
					continue;

				uint64_t value;
				sscanf(filename, "%" SCNx64, &value);
				seen_blobs.emplace(value, Entry{ i, size_t(s.m_uncomp_size) });
			}

			// In-place update the archive. Should we consider emitting a new archive instead?
			if (!mz_zip_writer_init_from_reader(&mz, path.c_str()))
			{
				LOGE("Failed to initialize ZIP writer from reader.\n");
				mz_zip_end(&mz);
				return;
			}

			alive = true;
		}
		else if (!readonly)
		{
			if (!mz_zip_writer_init_file(&mz, path.c_str(), 0))
			{
				LOGE("Failed to open ZIP archive for writing. Cannot serialize anything to disk.\n");
				return;
			}

			alive = true;
			seen_blobs.clear();
		}
	}

	bool read_entry(Hash hash, vector<uint8_t> &blob) override
	{
		if (!alive || !readonly)
			return false;

		auto itr = seen_blobs.find(hash);
		if (itr == end(seen_blobs))
			return false;

		blob.resize(itr->second.size);
		if (!mz_zip_reader_extract_to_mem(&mz, itr->second.index, blob.data(), blob.size(), 0))
		{
			LOGE("Failed to extract blob.\n");
			return false;
		}

		return true;
	}

	bool write_entry(Hash hash, const vector<uint8_t> &blob) override
	{
		if (!alive || readonly)
			return false;

		auto itr = seen_blobs.find(hash);
		if (itr != end(seen_blobs))
			return true;

		char str[16 + 1]; // 16 digits + null
		sprintf(str, "%016" PRIx64, hash);
		if (!mz_zip_writer_add_mem(&mz, str, blob.data(), blob.size(), MZ_NO_COMPRESSION))
		{
			LOGE("Failed to add blob to cache.\n");
			return false;
		}

		// The index is irrelevant, we're not going to read from this archive any time soon.
		seen_blobs.emplace(hash, Entry{ -1u, blob.size() });
		return true;
	}

	bool has_entry(Hash hash) override
	{
		auto itr = seen_blobs.find(hash);
		return itr != end(seen_blobs);
	}

	string path;
	mz_zip_archive mz;

	struct Entry
	{
		unsigned index;
		size_t size;
	};
	unordered_map<Hash, Entry> seen_blobs;
	bool alive = false;
	bool readonly;
};

unique_ptr<DatabaseInterface> create_zip_archive_database(const string &path, bool readonly)
{
	auto db = make_unique<ZipDatabase>(path, readonly);
	return move(db);
}

}