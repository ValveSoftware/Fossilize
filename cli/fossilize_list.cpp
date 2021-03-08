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

#include "fossilize_inttypes.h"
#include "fossilize_db.hpp"
#include "cli_parser.hpp"
#include "layer/utils.hpp"
#include <memory>
#include <vector>

using namespace Fossilize;
using namespace std;

static void print_help()
{
	LOGI("Usage: fossilize-list\n"
	     "\t<database path>\n"
	     "\t[--tag index]\n"
	     "\t[--size]\n");
}

int main(int argc, char **argv)
{
	CLICallbacks cbs;
	string db_path;
	unsigned tag_uint = 0;
	bool log_size = false;
	cbs.default_handler = [&](const char *path) { db_path = path; };
	cbs.add("--help", [&](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--tag", [&](CLIParser &parser) { tag_uint = parser.next_uint(); });
	cbs.add("--size", [&](CLIParser &) { log_size = true; });
	cbs.error_handler = [] { print_help(); };
	CLIParser parser(move(cbs), argc - 1, argv + 1);

	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (db_path.empty())
	{
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (tag_uint >= RESOURCE_COUNT)
	{
		LOGE("--tag (%u) is out of range.\n", tag_uint);
		return EXIT_FAILURE;
	}

	auto tag = static_cast<ResourceTag>(tag_uint);

	size_t hash_count = 0;
	if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
	{
		LOGE("Failed to get hashes.\n");
		return EXIT_FAILURE;
	}

	vector<Hash> hashes(hash_count);
	if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
	{
		LOGE("Failed to get shader module hashes.\n");
		return EXIT_FAILURE;
	}

	uint64_t compressed_total_size = 0;
	uint64_t uncompressed_total_size = 0;
	for (auto hash : hashes)
	{
		if (log_size)
		{
			size_t compressed_blob_size = 0;
			size_t uncompressed_blob_size = 0;
			if (!input_db->read_entry(tag, hash, &compressed_blob_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT) ||
			    !input_db->read_entry(tag, hash, &uncompressed_blob_size, nullptr, 0))
			{
				LOGE("Failed to query blob size.\n");
				return EXIT_FAILURE;
			}
			compressed_total_size += compressed_blob_size;
			uncompressed_total_size += uncompressed_blob_size;
			printf("%016" PRIx64 " %u compressed bytes, %u uncompressed bytes\n", hash,
			       unsigned(compressed_blob_size), unsigned(uncompressed_blob_size));
		}
		else
			printf("%016" PRIx64 "\n", hash);
	}

	if (log_size)
	{
		printf("Total size (compressed): %" PRIu64 " bytes.\n", compressed_total_size);
		printf("Total size (uncompressed): %" PRIu64 " bytes.\n", uncompressed_total_size);
	}
}
