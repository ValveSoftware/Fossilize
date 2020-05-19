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
#include <memory>
#include <vector>
#include "layer/utils.hpp"
#include <cstdlib>

using namespace Fossilize;

static void print_help()
{
	LOGI("Usage: fossilize-convert-db input-db output-db\n");
}

int main(int argc, char *argv[])
{
	if (argc != 3)
	{
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(argv[1], DatabaseMode::ReadOnly));
	auto output_db = std::unique_ptr<DatabaseInterface>(create_database(argv[2], DatabaseMode::OverWrite));
	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (!output_db || !output_db->prepare())
	{
		LOGE("Failed to open database for writing: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	for (unsigned i = 0; i < RESOURCE_COUNT; i++)
	{
		auto tag = static_cast<ResourceTag>(i);

		size_t hash_count = 0;
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
			return EXIT_FAILURE;
		std::vector<Hash> hashes(hash_count);
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
			return EXIT_FAILURE;

		for (auto &hash : hashes)
		{
			size_t blob_size = 0;
			if (!input_db->read_entry(tag, hash, &blob_size, nullptr, PAYLOAD_READ_NO_FLAGS))
				return EXIT_FAILURE;
			std::vector<uint8_t> blob(blob_size);
			if (!input_db->read_entry(tag, hash, &blob_size, blob.data(), PAYLOAD_READ_NO_FLAGS))
				return EXIT_FAILURE;

			if (!output_db->write_entry(tag, hash, blob.data(), blob.size(),
					PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT |
					PAYLOAD_WRITE_COMPRESS_BIT |
					PAYLOAD_WRITE_BEST_COMPRESSION_BIT))
			{
				return EXIT_FAILURE;
			}
		}
	}
}
