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

#include "fossilize_external_replayer.hpp"

#if defined(_WIN32)
#include "fossilize_external_replayer_windows.hpp"
#elif (defined(__linux__) && !defined(__ANDROID__)) || defined(__APPLE__)
#include "fossilize_external_replayer_linux.hpp"
#else
#error "Unsupported platform."
#endif

namespace Fossilize
{
ExternalReplayer::ExternalReplayer()
{
	impl = new Impl;
}

ExternalReplayer::~ExternalReplayer()
{
	delete impl;
}

int ExternalReplayer::wait()
{
	return impl->wait();
}

bool ExternalReplayer::kill()
{
	return impl->kill();
}

uintptr_t ExternalReplayer::get_process_handle() const
{
	return impl->get_process_handle();
}

bool ExternalReplayer::start(const Options &options)
{
	return impl->start(options);
}

ExternalReplayer::PollResult ExternalReplayer::poll_progress(Progress &progress)
{
	return impl->poll_progress(progress);
}

bool ExternalReplayer::is_process_complete(int *return_status)
{
	return impl->is_process_complete(return_status);
}

bool ExternalReplayer::get_faulty_spirv_modules(size_t *num_hashes, Hash *hashes)
{
	return impl->get_faulty_spirv_modules(num_hashes, hashes);
}
}
