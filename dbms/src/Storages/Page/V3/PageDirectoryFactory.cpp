// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Storages/Page/V3/PageDirectory.h>
#include <Storages/Page/V3/PageDirectoryFactory.h>
#include <Storages/Page/V3/PageEntriesEdit.h>
#include <Storages/Page/V3/WAL/WALReader.h>
#include <Storages/Page/V3/WALStore.h>

#include <memory>

namespace DB
{
namespace ErrorCodes
{
extern const int PS_DIR_APPLY_INVALID_STATUS;
} // namespace ErrorCodes
namespace PS::V3
{
PageDirectoryPtr PageDirectoryFactory::create(String storage_name, FileProviderPtr & file_provider, PSDiskDelegatorPtr & delegator, WALStore::Config config)
{
    auto [wal, reader] = WALStore::create(storage_name, file_provider, delegator, config);
    PageDirectoryPtr dir = std::make_unique<PageDirectory>(std::move(storage_name), std::move(wal), config.max_persisted_log_files);
    loadFromDisk(dir, std::move(reader));

    // Reset the `sequence` to the maximum of persisted.
    dir->sequence = max_applied_ver.sequence;

    // After restoring from the disk, we need cleanup all invalid entries in memory, or it will
    // try to run GC again on some entries that are already marked as invalid in BlobStore.
    dir->gcInMemEntries();

    if (blob_stats)
    {
        // After all entries restored to `mvcc_table_directory`, only apply
        // the latest entry to `blob_stats`, or we may meet error since
        // some entries may be removed in memory but not get compacted
        // in the log file.
        for (const auto & [page_id, entries] : dir->mvcc_table_directory)
        {
            (void)page_id;

            // We should restore the entry to `blob_stats` even if it is marked as "deleted",
            // or we will mistakenly reuse the space to write other blobs down into that space.
            // So we need to use `getLastEntry` instead of `getEntry(version)` here.
            if (auto entry = entries->getLastEntry(); entry)
            {
                blob_stats->restoreByEntry(*entry);
            }
        }

        blob_stats->restore();
    }

    // TODO: After restored ends, set the last offset of log file for `wal`
    return dir;
}

PageDirectoryPtr PageDirectoryFactory::createFromEdit(String storage_name, FileProviderPtr & file_provider, PSDiskDelegatorPtr & delegator, const PageEntriesEdit & edit)
{
    auto [wal, reader] = WALStore::create(storage_name, file_provider, delegator, WALStore::Config());
    (void)reader;
    PageDirectoryPtr dir = std::make_unique<PageDirectory>(std::move(storage_name), std::move(wal));
    loadEdit(dir, edit);
    // Reset the `sequence` to the maximum of persisted.
    dir->sequence = max_applied_ver.sequence;

    // After restoring from the disk, we need cleanup all invalid entries in memory, or it will
    // try to run GC again on some entries that are already marked as invalid in BlobStore.
    dir->gcInMemEntries();

    if (blob_stats)
    {
        // After all entries restored to `mvcc_table_directory`, only apply
        // the latest entry to `blob_stats`, or we may meet error since
        // some entries may be removed in memory but not get compacted
        // in the log file.
        for (const auto & [page_id, entries] : dir->mvcc_table_directory)
        {
            (void)page_id;

            // We should restore the entry to `blob_stats` even if it is marked as "deleted",
            // or we will mistakenly reuse the space to write other blobs down into that space.
            // So we need to use `getLastEntry` instead of `getEntry(version)` here.
            if (auto entry = entries->getLastEntry(); entry)
            {
                blob_stats->restoreByEntry(*entry);
            }
        }

        blob_stats->restore();
    }

    return dir;
}

void PageDirectoryFactory::loadEdit(const PageDirectoryPtr & dir, const PageEntriesEdit & edit)
{
    for (const auto & r : edit.getRecords())
    {
        if (max_applied_ver < r.version)
            max_applied_ver = r.version;
        max_applied_page_id = std::max(r.page_id, max_applied_page_id);

        // We can not avoid page id from being reused under some corner situation. Try to do gcInMemEntries
        // and apply again to resolve the error.
        if (bool ok = applyRecord(dir, r, /*throw_on_error*/ false); unlikely(!ok))
        {
            dir->gcInMemEntries();
            applyRecord(dir, r, /*throw_on_error*/ true);
            LOG_FMT_INFO(DB::Logger::get("PageDirectoryFactory"), "resolve from error status done, continue to restore");
        }
    }
}

bool PageDirectoryFactory::applyRecord(
    const PageDirectoryPtr & dir,
    const PageEntriesEdit::EditRecord & r,
    bool throw_on_error)
{
    auto [iter, created] = dir->mvcc_table_directory.insert(std::make_pair(r.page_id, nullptr));
    if (created)
    {
        iter->second = std::make_shared<VersionedPageEntries>();
    }

    const auto & version_list = iter->second;
    const auto & restored_version = r.version;
    try
    {
        switch (r.type)
        {
        case EditRecordType::VAR_EXTERNAL:
        case EditRecordType::VAR_REF:
        {
            auto holder = version_list->fromRestored(r);
            if (holder)
            {
                *holder = r.page_id;
                dir->external_ids.emplace_back(std::weak_ptr<PageIdV3Internal>(holder));
            }
            break;
        }
        case EditRecordType::VAR_ENTRY:
            version_list->fromRestored(r);
            break;
        case EditRecordType::PUT_EXTERNAL:
        {
            auto holder = version_list->createNewExternal(restored_version);
            if (holder)
            {
                *holder = r.page_id;
                dir->external_ids.emplace_back(std::weak_ptr<PageIdV3Internal>(holder));
            }
            break;
        }
        case EditRecordType::PUT:
            version_list->createNewEntry(restored_version, r.entry);
            break;
        case EditRecordType::DEL:
        case EditRecordType::VAR_DELETE: // nothing different from `DEL`
            version_list->createDelete(restored_version);
            break;
        case EditRecordType::REF:
            PageDirectory::applyRefEditRecord(
                dir->mvcc_table_directory,
                version_list,
                r,
                restored_version);
            break;
        case EditRecordType::UPSERT:
            version_list->createNewEntry(restored_version, r.entry);
            break;
        }
    }
    catch (DB::Exception & e)
    {
        e.addMessage(fmt::format(" [type={}] [page_id={}] [ver={}]", r.type, r.page_id, restored_version));
        if (throw_on_error || e.code() != ErrorCodes::PS_DIR_APPLY_INVALID_STATUS)
        {
            throw e;
        }
        LOG_FMT_WARNING(DB::Logger::get("PageDirectoryFactory"), "try to resolve error during restore: {}", e.message());
        return false;
    }
    return true;
}

void PageDirectoryFactory::loadFromDisk(const PageDirectoryPtr & dir, WALStoreReaderPtr && reader)
{
    while (reader->remained())
    {
        auto [ok, edit] = reader->next();
        if (!ok)
        {
            // TODO: Handle error, some error could be ignored.
            // If the file happened to some error,
            // should truncate it to throw away incomplete data.
            reader->throwIfError();
            // else it just run to the end of file.
            break;
        }

        // apply the edit read
        loadEdit(dir, edit);
    }
}
} // namespace PS::V3
} // namespace DB