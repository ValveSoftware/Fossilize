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
#include <algorithm>

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

void ExternalReplayer::compute_condensed_progress(const Progress &progress, unsigned &completed, unsigned &total)
{
	// Some pipelines might be compiled twice if they are derivable pipelines especially when using multi-process replays.
	// There are also shenanigans when pipelines are compiled multiple times as a result of crash recoveries.
	// We do not know ahead of time how many modules we are going to compile from the archive.
	// This depends entirely on which modules the pipelines refer to.
	// Due to all these quirks, it is somewhat complicated to provide an accurate metric on completion.

	// As modules are pulled in, they increase the total progress.
	// Since it may look like progress is moving backwards in this scenario,
	// it's better to heavily discount shader module progress.
	constexpr unsigned ModuleWeightDivider = 10;
	unsigned weighted_total_modules = (progress.total_modules + ModuleWeightDivider - 1) / ModuleWeightDivider;

	// Skipped pipelines (i.e. we only did parsing) mostly lower the total,
	// but to get some semblance of forward progress in the total,
	// increment the progress by 1 for every 100 skipped pipelines.
	// This is mostly relevant for cached pipeline replays.
	constexpr unsigned SkipWeightDivider = 100;

	// If we have crashes or other unexpected behavior, these values might increase beyond the expected value.
	// Just clamp it to never report obviously wrong values.
	// The only glitch we risk is that we're stuck at "100%" a bit longer,
	// but UI can always report something here when we know we're not done yet.

	// To aim for good forward progress metrics, skipped or cached pipelines where we're not doing anything useful should subtract from the total instead.
	unsigned skipped_graphics = progress.graphics.cached + progress.graphics.skipped;
	unsigned total_work_graphics = progress.total_graphics_pipeline_blobs -
	                               (std::min)(progress.total_graphics_pipeline_blobs, skipped_graphics);
	unsigned weighted_skipped_graphics = (skipped_graphics + SkipWeightDivider - 1) / SkipWeightDivider;

	unsigned skipped_compute = progress.compute.cached + progress.compute.skipped;
	unsigned total_work_compute = progress.total_compute_pipeline_blobs -
	                              (std::min)(progress.total_compute_pipeline_blobs, skipped_compute);
	unsigned weighted_skipped_compute = (skipped_compute + SkipWeightDivider - 1) / SkipWeightDivider;

	unsigned skipped_raytracing = progress.raytracing.cached + progress.raytracing.skipped;
	unsigned total_work_raytracing = progress.total_raytracing_pipeline_blobs -
	                                 (std::min)(progress.total_raytracing_pipeline_blobs, skipped_raytracing);
	unsigned weighted_skipped_raytracing = (skipped_raytracing + SkipWeightDivider - 1) / SkipWeightDivider;

	unsigned decompressed_modules = (std::min)(progress.completed_modules + progress.module_validation_failures +
	                                           progress.banned_modules + progress.missing_modules, progress.total_modules);
	unsigned weighted_decompressed_modules = (decompressed_modules + ModuleWeightDivider - 1) / ModuleWeightDivider;

	unsigned compiled_graphics = (std::min)(progress.graphics.completed, total_work_graphics);
	unsigned compiled_compute = (std::min)(progress.compute.completed, total_work_compute);
	unsigned compiled_raytracing = (std::min)(progress.raytracing.completed, total_work_raytracing);

	completed = compiled_graphics + compiled_compute + compiled_raytracing + weighted_decompressed_modules;
	total = total_work_graphics + total_work_compute + total_work_raytracing + weighted_total_modules;

	unsigned total_weighted_skipped = weighted_skipped_graphics + weighted_skipped_compute + weighted_skipped_raytracing;
	completed += total_weighted_skipped;
	total += total_weighted_skipped;
}

bool ExternalReplayer::is_process_complete(int *return_status)
{
	return impl->is_process_complete(return_status);
}

bool ExternalReplayer::get_faulty_spirv_modules(size_t *num_hashes, Hash *hashes) const
{
	return impl->get_faulty_spirv_modules(num_hashes, hashes);
}

bool ExternalReplayer::get_faulty_graphics_pipelines(size_t *num_hashes, unsigned *indices, Hash *hashes) const
{
	return impl->get_faulty_graphics_pipelines(num_hashes, indices, hashes);
}

bool ExternalReplayer::get_faulty_compute_pipelines(size_t *num_hashes, unsigned *indices, Hash *hashes) const
{
	return impl->get_faulty_compute_pipelines(num_hashes, indices, hashes);
}

bool ExternalReplayer::get_faulty_raytracing_pipelines(size_t *num_hashes, unsigned *indices, Hash *hashes) const
{
	return impl->get_faulty_raytracing_pipelines(num_hashes, indices, hashes);
}

bool ExternalReplayer::get_graphics_failed_validation(size_t *num_hashes, Hash *hashes) const
{
	return impl->get_graphics_failed_validation(num_hashes, hashes);
}

bool ExternalReplayer::get_compute_failed_validation(size_t *num_hashes, Hash *hashes) const
{
	return impl->get_compute_failed_validation(num_hashes, hashes);
}

bool ExternalReplayer::get_raytracing_failed_validation(size_t *num_hashes, Hash *hashes) const
{
	return impl->get_raytracing_failed_validation(num_hashes, hashes);
}

bool ExternalReplayer::poll_memory_usage(uint32_t *num_processes, ProcessStats *stats) const
{
	return impl->poll_memory_usage(num_processes, stats);
}

bool ExternalReplayer::poll_global_resource_usage(GlobalResourceUsage &stats) const
{
	return impl->poll_global_resource_usage(stats);
}

bool ExternalReplayer::send_message(const char *msg)
{
	return impl->send_message(msg);
}
}
