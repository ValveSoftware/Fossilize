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
#include <vector>
#include "layer/utils.hpp"
#include <cstdlib>
#include "cli_parser.hpp"

using namespace Fossilize;

static void print_help()
{
	LOGI("Usage: fossilize-merge-db [--last-use] append.foz [input1.foz] [input2.foz] ...\n");
}

int main(int argc, char **argv)
{
	std::vector<const char *> inputs;
	bool last_use = false;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { inputs.push_back(arg); };
	cbs.add("--last-use", [&](CLIParser &) { last_use = true; });
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (inputs.size() < 2)
		return EXIT_FAILURE;

	if (last_use)
	{
		if (!merge_concurrent_databases_last_use(inputs.front(), inputs.data() + 1, inputs.size() - 1))
			return EXIT_FAILURE;
	}
	else
	{
		if (!merge_concurrent_databases(inputs.front(), inputs.data() + 1, inputs.size() - 1))
			return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
