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

	// Allows read_entry to be called concurrently from multiple threads.
	// Might cause locking when reading from database depending on implementation.
	// Decompression if needed is always lock-free.
	// *NOTE*: Only tested with the Fossilize database format.
	PAYLOAD_READ_CONCURRENT_BIT = 1 << 1,

	PAYLOAD_READ_MAX_ENUM = 0x7fffffff
};
using PayloadWriteFlags = uint32_t;
using PayloadReadFlags = uint32_t;

enum class DatabaseMode
{
	Append,
	ReadOnly,
	// Special mode for the concurrent database interface where we can read the read-only portion only.
	AppendWithReadOnlyAccess,
	OverWrite,
	// In the stream database backend, this will ensure that the database is exclusively created.
	// For other backends, this is an alias for OverWrite
	ExclusiveOverWrite
};

struct ExportedMetadataHeader;

// This is an interface to interact with an external database for blob modules.
// It is is a simple database with key + blob.
// NOTE: The database is NOT thread-safe.
class DatabaseInterface
{
public:
	DatabaseInterface(DatabaseMode mode);
	virtual ~DatabaseInterface();

	// Loads a white-list and/or blacklist database which filters
	// which entries are retrieved through get_hash_list_for_resource_tag.
	// The database is a stream archive (.foz) with 0-sized blobs for tags
	// SHADER_MODULE, COMPUTE_PIPELINE, GRAPHICS_PIPELINE.
	// This must be called before prepare().
	// Can only be used for ReadOnly database mode.
	bool load_whitelist_database(const char *path);
	bool load_blacklist_database(const char *path);

	// When using a whitelist in load_whitelist_database,
	// only consider checking whitelist for a tag
	// where the tag'th bit is set in mask.
	// By default this mask is set for shader modules and graphics pipelines.
	void set_whitelist_tag_mask(uint32_t mask);

	// Useful if also replaying a database which is known to contain valid data.
	//
	// Currently only supported for the concurrent database.
	// This must be called before prepare().
	// Can only be used for ReadOnly database mode.
	//
	// In this mode, any resource in the denoted database will always pass the whitelist test.
	// Index 0 refers to the primary read-only database,
	// and index 1 and up refer to extra readonly databases which are passed in.
	//
	// Can be called multiple times to whitelist multiple databases.
	// If a subdata index does not prepare() successfully, the database entries for that index
	// will not be promoted to the whitelist.
	//
	// The database is considered sensitive to whitelist checking
	// if either load_whitelist_database is successfully called,
	// or at least one call to promote_sub_database_to_whitelist() is used
	// (even if that sub database fails to prepare()).
	void promote_sub_database_to_whitelist(unsigned index);

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

	// Ensures all file writes are flushed, ala fflush(). Might be noop depending on the implementation.
	virtual void flush() = 0;

	virtual const char *get_db_path_for_hash(ResourceTag tag, Hash hash) = 0;

	// For the concurrent database, gets the sub-database, otherwise returns nullptr.
	// Index 0 refers to the primary read-only database,
	// and index 1 and up refer to extra readonly databases which are passed in.
	// The sub-database is not subject to white- or blacklisting.
	// This can only be used in read-only mode.
	// For non-concurrent database types, index 0 returns this, nullptr otherwise.
	virtual DatabaseInterface *get_sub_database(unsigned index);
	virtual bool has_sub_databases();

	// This is a special purpose feature which allows us to parse a StreamArchive once,
	// build optimized metadata structures for it, which can then be shared with other processes.
	// Used primarily by the multi-process replayer.
	// This is an int FD on Unix-likes and a FileMapping HANDLE on Win32.
	// Can only be used on ReadOnly StreamArchive databases (or concurrent variant).
	// Caller is responsible for closing these handles.
	// The handle is already created with sharing enabled.
	// The format of the encoded data is application specific and is only expected to be
	// compatible with the exact same commit of Fossilize.
	// Name is an os-dependent name. You can obtain a unique name from get_unique_os_export_name().
	// On Unix-likes, the SHM name is unlinked immediately, FD should be inherited through a fork().
	// On Windows, the FileMapping handle must be opened explicitly in child processes.
	intptr_t export_metadata_to_os_handle(const char *name);
	enum { OSHandleNameSize = 64 };
	// Gets a name for passing into export_metadata_to_os_handle. size should be equal to OSHandleNameSize.
	static void get_unique_os_export_name(char *buffer, size_t size);
	static bool metadata_handle_is_valid(intptr_t handle);
	static intptr_t invalid_metadata_handle();

	// Takes ownership of the handle if call succeeds. Call before prepare().
	// This way, parsing can be skipped and metadata can be read from a shared memory block instead.
	// Will only work if the database setup is exactly the same as the setup that was used for
	// export_metadata_to_os_handle().
	// Importing metadata cannot be used together with white- or blacklists.
	// If needed, those must be used before exporting metadata.
	bool import_metadata_from_os_handle(intptr_t handle);

	// Internal details.
	virtual size_t compute_exported_metadata_size() const;
	virtual bool write_exported_metadata(void *data, size_t size) const;
	void add_imported_metadata(const ExportedMetadataHeader *header);

	// For bucketization of archives.
	// This only works with concurrent databases in Append or Overwrite mode.
	// See further comments after create_concurrent_database().
	virtual bool set_bucket_path(const char *bucket_dirname, const char *bucket_basename);

	// Request termination of prepare() which can take a long time for very
	// large archives.  This is useful if running prepare() on a thread and the need
	// arises to stop loading the database.
	static void request_shutdown();

protected:
	bool test_resource_filter(ResourceTag tag, Hash hash) const;
	bool add_to_implicit_whitelist(DatabaseInterface &iface);

	struct Impl;
	Impl *impl;
};

DatabaseInterface *create_dumb_folder_database(const char *directory_path, DatabaseMode mode);
DatabaseInterface *create_zip_archive_database(const char *path, DatabaseMode mode);
DatabaseInterface *create_stream_archive_database(const char *path, DatabaseMode mode);
DatabaseInterface *create_database(const char *path, DatabaseMode mode);

// This is a special kind of database which can be used from multiple independent processes and splits out the database
// into a read-only part and a write-only part, which is unique for each instance of this database.
// base_path.foz is the read-only database. If it does not exist, it will not be written to either.
// If there are any writes to the database which do not already exist in the read-only database, a new
// database will be created at base_path.%d.foz, where %d is a unique, monotonically increasing index starting at 1.
// Exclusive file open mechanisms are used to ensure correctness when multiple processes are present.
//
// The Fossilize layer will make sure access to a single instance of DatabaseInterface is serialized to one thread.
//
// Mode can be ReadOnly, Append, AppendWithReadOnly or Overwrite. Any other mode will fail.
//
// It is possible to specify some extra database paths which are treated as read-only.
// In ReadOnly mode, all entries in these databases are assumed to be part of the read-only database base_path.foz,
// and thus will not trigger creation of a new database.
// Similarly, in append mode, the entries in the extra databases are assumed to be part of the base_path.foz database.
// If any database in extra_read_only_database_paths does not ->prepare() correctly, it is simply ignored.
// base_path may be nullptr if mode is ReadOnly. In this case, the read-only database from base_path.foz is ignored.
DatabaseInterface *create_concurrent_database(const char *base_path, DatabaseMode mode,
                                              const char * const *extra_read_only_database_paths,
                                              size_t num_extra_read_only_database_paths);

// Like create_concurrent_database, except encoded_read_only_database_paths
// contains a list of paths delimited by ';'. E.g. "foo;bar;baz". Suitable to use directly with getenv().
// encoded_read_only_database_paths may be nullptr, which is treated as no extra paths.
// On non-Windows systems, ':' can also be used to delimit to match $PATH behavior.
// base_path may be nullptr if mode is ReadOnly. In this case, the read-only database from base_path.foz is ignored.
DatabaseInterface *create_concurrent_database_with_encoded_extra_paths(const char *base_path, DatabaseMode mode,
                                                                       const char *encoded_read_only_database_paths);

// Merges stream archives found in source_paths into append_database_path.
bool merge_concurrent_databases(const char *append_database_path,
                                const char * const *source_paths, size_t num_source_paths,
                                bool skip_missing_inputs = false);

// Merges stream archives found in source_paths into append_database_path.
// When there are duplicates in append database and any other database, picks the entry with the highest 8 byte timestamp payload.
bool merge_concurrent_databases_last_use(const char *append_database_path,
                                         const char * const *source_paths, size_t num_source_paths,
                                         bool skip_missing_inputs = false);

// For set_bucket_path() behavior on a concurrent database:
// Must be called before prepare().
//
// In this style, we intend to sort archives based on some global "variant" state when recording.
// This can be very useful when an application has many different shader variants based on vendor ID,
// engine version, enabled device features, etc, and the intention here is to completely
// separate archives based on these keys.
//
// When prepare() is called and bucket_dirname is non-empty, the $base_path for the concurrent archive is adjusted,
// and the implementation will create a new directory $base_path.$bucket_dirname/.
// Directories are not created recursively.
//
// Instead of using $base_path.foz (read-only part) and $base_path.*.foz (write-only part), we use
// $base_path.$bucket_dirname/$bucket_basename.foz (read-only) and
// $base_path.$bucket_dirname/$bucket_basename.*.foz (write-only).
//
// A separate empty file $base_path.$bucket_dirname/TOUCH is created and/or touched in prepare()
// to update last access time,
// which lets external tools deduce which buckets are actually in use by the application in question.
//
// extra_read_only_database_paths are not modified by buckets directly,
// but to make the system work well with buckets, it is possible to encode a relative path
// to the basedir of the read-only part of the bucket as, e.g.:
// "$bucketdir/static_archive.foz"
// The relative path is always honored, even if bucket paths are not enabled.
// If the selected read-only path is e.g.:
// $base_path.$bucket_dirname/$bucket_basename.foz
// the resulting path for the extra read only archive is:
// $base_path.$bucket_dirname/static_archive.foz
}
