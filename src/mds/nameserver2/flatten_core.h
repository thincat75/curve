/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 2023-07-24
 * Author: xuchaojie
 */

#ifndef SRC_MDS_NAMESERVER2_FLATTEN_CORE_H_
#define SRC_MDS_NAMESERVER2_FLATTEN_CORE_H_

#include <memory>

#include "src/mds/nameserver2/task_progress.h"
#include "src/mds/nameserver2/namespace_storage.h"
#include "src/mds/chunkserverclient/copyset_client.h"
#include "src/mds/nameserver2/file_lock.h"
#include "src/common/task_tracker.h"

using curve::mds::chunkserverclient::FlattenChunkContext;
using ::curve::mds::chunkserverclient::CopysetClient;
using ::curve::common::TaskTracker;
using ::curve::common::ContextTaskTracker;

namespace curve {
namespace mds {

using FlattenChunkTaskTracker = 
    ContextTaskTracker<std::shared_ptr<FlattenChunkContext>>;

class FlattenCore {
 public:
    explicit FlattenCore(const std::shared_ptr<NameServerStorage> &storage, 
        const std::shared_ptr<CopysetClient> &copysetClient,
        FileLockManager *fileLockManager)
        : storage_(storage), copysetClient_(copysetClient),
          fileLockManager_(fileLockManager) {}

    void DoFlatten(
        const std::string &fileName,
        const FileInfo &fileInfo,
        const FileInfo &originFileInfo,
        const FileInfo &virtualFileInfo,
        TaskProgress *progress);

 private:
    int StartAsyncFlattenChunk(
        const std::shared_ptr<FlattenChunkTaskTracker> &tracker,
        const std::shared_ptr<FlattenChunkContext> &context);

    int WaitAsycnFlattenChunkDone(
        const std::shared_ptr<FlattenChunkTaskTracker> &tracker,
        uint32_t *completeChunkNum);

 private:
    uint32_t flattenChunkConcurrency_;

    std::shared_ptr<NameServerStorage> storage_;
    std::shared_ptr<CopysetClient> copysetClient_;
    FileLockManager *fileLockManager_;
};

}  // namespace mds
}  // namespace curve


#endif  // SRC_MDS_NAMESERVER2_FLATTEN_CORE_H_
