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
 * Created Date: Saturday October 13th 2018
 * Author: hzsunjianliang
 */

#include <glog/logging.h>
#include "src/mds/nameserver2/chunk_allocator.h"
#include "proto/nameserver2.pb.h"


namespace curve {
namespace mds {
bool ChunkSegmentAllocatorImpl::AllocateChunkSegment(FileType type,
        SegmentSizeType segmentSize, ChunkSizeType chunkSize,
        const std::string& pstName, offset_t offset,
        PageFileSegment *segment)  {
        if (segment == nullptr) {
            LOG(ERROR) << "segment pointer is null";
            return false;
        }

        if (offset % segmentSize != 0) {
            LOG(ERROR) << "offset not align with segmentsize";
            return false;
        }
        if (chunkSize == 0 || chunkSize > segmentSize ||
                segmentSize % chunkSize != 0) {
            LOG(ERROR) << "chunkSize not align with segmentsize";
            return false;
        }

        segment->set_chunksize(chunkSize);
        segment->set_segmentsize(segmentSize);
        segment->set_startoffset(offset);

        // allocate chunks
        uint32_t chunkNum = segmentSize/chunkSize;
        std::vector<CopysetIdInfo> copysets;
        if (!topologyChunkAllocator_->
                AllocateChunkRoundRobinInSingleLogicalPool(
                type, pstName, chunkNum, chunkSize, &copysets)) {
            LOG(ERROR) << "AllocateChunkRoundRobinInSingleLogicalPool error";
            return false;
        }
        if (copysets.size() != chunkNum) {
            LOG(ERROR) << "AllocateChunk return size error";
            return false;
        }
        auto logicalpoolId = copysets[0].logicalPoolId;
        for (auto i = 0; i < copysets.size(); i++) {
            if (copysets[i].logicalPoolId !=  logicalpoolId) {
                LOG(ERROR) << "Allocate Copysets id not same, copysets["
                            << i << "] = "
                            << copysets[i].logicalPoolId
                            << ", correct =" << logicalpoolId;
                return false;
            }
        }

        segment->set_logicalpoolid(logicalpoolId);

        for (uint32_t i = 0; i < chunkNum ; i++) {
            PageFileChunkInfo* chunkinfo = segment->add_chunks();

            ChunkID chunkID;
            if (!chunkIDGenerator_->GenChunkID(&chunkID)) {
                LOG(ERROR) << "allocate error";
                return false;
            }
            chunkinfo->set_chunkid(chunkID);
            chunkinfo->set_copysetid(copysets[i].copySetId);
        }
        return true;
}

bool ChunkSegmentAllocatorImpl::CloneChunkSegment(
    const std::string &srcFileName,
    uint64_t srcFileId,
    const PageFileSegment &srcSegment,
    PageFileSegment *segment) {
    // check logical space enough
    std::list<PoolIdType> logicalPools;
    logicalPools.push_back(static_cast<PoolIdType>(srcSegment.logicalpoolid()));
    std::map<PoolIdType, double> enoughSpacePools;
    topologyChunkAllocator_->GetRemainingSpaceInLogicalPool(
            logicalPools, &enoughSpacePools);
    if ((enoughSpacePools.empty()) || 
        (enoughSpacePools[srcSegment.logicalpoolid()] < 
            srcSegment.segmentsize())) {
        LOG(ERROR) << "logical id: " << srcSegment.logicalpoolid()
                   << ", space not enough";
        return false;
    }
    if (srcSegment.has_originfileid()) {
        segment->set_originfileid(srcSegment.originfileid());
    } else {
        segment->set_originfileid(srcFileId);
    }
    segment->set_logicalpoolid(srcSegment.logicalpoolid());
    segment->set_segmentsize(srcSegment.segmentsize());
    segment->set_chunksize(srcSegment.chunksize());
    segment->set_startoffset(srcSegment.startoffset());
    for (uint32_t i = 0; i < srcSegment.chunks_size(); i++) {
        PageFileChunkInfo* chunkinfo = segment->add_chunks();

        ChunkID chunkID;
        if (!chunkIDGenerator_->GenChunkID(&chunkID)) {
            LOG(ERROR) << "allocate error";
            return false;
        }
        chunkinfo->set_chunkid(chunkID);
        chunkinfo->set_copysetid(srcSegment.chunks(i).copysetid());
    }
    return true;
}

}   // namespace mds
}   // namespace curve
