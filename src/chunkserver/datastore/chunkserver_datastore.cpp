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
 * File Created: Wednesday, 5th September 2018 8:04:03 pm
 * Author: yangyaokai
 */

#include <gflags/gflags.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>

#include "src/chunkserver/datastore/chunkserver_datastore.h"
#include "src/chunkserver/datastore/filename_operator.h"
#include "src/common/location_operator.h"

namespace curve {
namespace chunkserver {

CSDataStore::CSDataStore(std::shared_ptr<LocalFileSystem> lfs,
                         std::shared_ptr<FilePool> chunkFilePool,
                         const DataStoreOptions& options)
    : chunkSize_(options.chunkSize),
      pageSize_(options.pageSize),
      baseDir_(options.baseDir),
      locationLimit_(options.locationLimit),
      chunkFilePool_(chunkFilePool),
      lfs_(lfs),
      enableOdsyncWhenOpenChunkFile_(options.enableOdsyncWhenOpenChunkFile) {
    CHECK(!baseDir_.empty()) << "Create datastore failed";
    CHECK(lfs_ != nullptr) << "Create datastore failed";
    CHECK(chunkFilePool_ != nullptr) << "Create datastore failed";
}

CSDataStore::~CSDataStore() {
}

bool CSDataStore::Initialize() {
    // Make sure the baseDir directory exists
    if (!lfs_->DirExists(baseDir_.c_str())) {
        int rc = lfs_->Mkdir(baseDir_.c_str());
        if (rc < 0) {
            LOG(ERROR) << "Create " << baseDir_ << " failed.";
            return false;
        }
    }

    vector<string> files;
    int rc = lfs_->List(baseDir_, &files);
    if (rc < 0) {
        LOG(ERROR) << "List " << baseDir_ << " failed.";
        return false;
    }

    // If loaded before, reload here
    metaCache_.Clear();
    cloneCache_.Clear();
    metric_ = std::make_shared<DataStoreMetric>();
    for (size_t i = 0; i < files.size(); ++i) {
        FileNameOperator::FileInfo info =
            FileNameOperator::ParseFileName(files[i]);
        if (info.type == FileNameOperator::FileType::CHUNK) {
            // If the chunk file has not been loaded yet, load it to metaCache
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed: " << files[i];
                return false;
            }
        } else if (info.type == FileNameOperator::FileType::SNAPSHOT) {
            string chunkFilePath = baseDir_ + "/" +
                        FileNameOperator::GenerateChunkFileName(info.id);

            // If the chunk file does not exist, print the log
            if (!lfs_->FileExists(chunkFilePath)) {
                LOG(WARNING) << "Can't find snapshot "
                             << files[i] << "' chunk.";
                continue;
            }
            // If the chunk file exists, load the chunk file to metaCache first
            CSErrorCode errorCode = loadChunkFile(info.id);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load chunk file failed.";
                return false;
            }

            // Load snapshot to memory
            errorCode = metaCache_.Get(info.id)->LoadSnapshot(info.sn);
            if (errorCode != CSErrorCode::Success) {
                LOG(ERROR) << "Load snapshot failed.";
                return false;
            }
        } else {
            LOG(WARNING) << "Unknown file: " << files[i];
        }
    }
    LOG(INFO) << "Initialize data store success.";
    return true;
}

CSErrorCode CSDataStore::DeleteChunk(ChunkID id, SequenceNum sn, std::shared_ptr<SnapContext> ctx) {
    if (ctx != nullptr && !ctx->empty()) {
        LOG(WARNING) << "Delete chunk file failed: snapshot exists."
                     << "ChunkID = " << id;
        return CSErrorCode::SnapshotExistError;
    }

    auto chunkFile = metaCache_.Get(id);
    if (chunkFile != nullptr) {
        CSErrorCode errorCode = chunkFile->Delete(sn);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Delete chunk file failed."
                         << "ChunkID = " << id;
            return errorCode;
        }
        metaCache_.Remove(id);
        
        uint64_t cloneno = chunkFile->getCloneNumber();
        if (cloneno > 0) {
            cloneCache_.Remove(id, cloneno);
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::DeleteSnapshotChunk(
    ChunkID id, SequenceNum snapSn, std::shared_ptr<SnapContext> ctx) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile != nullptr) {
        CSErrorCode errorCode = chunkFile->DeleteSnapshot(snapSn, ctx);  // NOLINT
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Delete snapshot chunk or correct sn failed."
                         << "ChunkID = " << id
                         << ", snapSn = " << snapSn;
            return errorCode;
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::ReadChunk(ChunkID id,
                                   SequenceNum sn,
                                   char * buf,
                                   off_t offset,
                                   size_t length) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }

    CSErrorCode errorCode = chunkFile->Read(buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Read chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}


struct CloneInfos CSDataStore::getParentClone (std::vector<struct CloneInfos>& clones, uint64_t cloneNo) {
    struct CloneInfos prev_clone;
    //use iterator to traverse the vector
    prev_clone = *clones.begin();
    for (auto it = clones.begin(); it != clones.end(); it++) {
        if (it->cloneNo == cloneNo) {
            return prev_clone;
        }
        prev_clone = *it;
    }

    return prev_clone;
}

// searchChunkForObj is a func to search the obj to find the obj in < chunkfile, sn, snapshot>
void CSDataStore::searchChunkForObj (SequenceNum sn, 
                                    std::vector<File_ObjectInfoPtr>& file_objs, 
                                    uint32_t beginIndex, uint32_t endIndex, 
                                    std::unique_ptr<CloneContext>& ctx) {

    std::vector<BitRange> bitRanges;
    std::vector<BitRange> notInMapBitRanges;

    CSChunkFilePtr cloneFile = nullptr;
    CSChunkFilePtr rootChunkFile = nullptr;

    bool isFinish = false;

    BitRange objRange;
    objRange.beginIndex = beginIndex;
    objRange.endIndex = endIndex;
    bitRanges.push_back(objRange);

    SequenceNum cloneSn = sn;
    uint64_t cloneParentNo = 0;
    uint64_t cloneNo = ctx->cloneNo;
    struct CloneInfos tmpclone;

    if (0 != ctx->cloneNo) {
        rootChunkFile = metaCache_.Get(ctx->rootId);
        cloneFile = cloneCache_.Get(ctx->rootId, ctx->cloneNo);
        while (nullptr == cloneFile) {
            tmpclone = getParentClone (ctx->clones, cloneNo);
            cloneParentNo = tmpclone.cloneNo;
            cloneSn = tmpclone.cloneSn;
            cloneNo = cloneParentNo;
            if (0 == cloneParentNo) {
                break;
            }
            cloneFile = cloneCache_.Get(ctx->rootId, cloneParentNo);
        }
    }

    if (nullptr == cloneFile) { //must be zero, not any clone chunk left, just search the chunkfile
        assert (0 == cloneParentNo);
        if (nullptr != rootChunkFile) {
            std::unique_ptr<File_ObjectInfo> fobs(new File_ObjectInfo());
            fobs->obj_infos.reserve(OBJECTINFO_SIZE);
            isFinish = rootChunkFile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, fobs->obj_infos);
            if (true != fobs->obj_infos.empty()) {
                fobs->fileptr = rootChunkFile;
                file_objs.push_back (std::move(fobs));
            }
            assert (isFinish == true);

            return;
        } else { //not any clonefile and root file exists, just fill with zero
            std::unique_ptr<File_ObjectInfo> fobs(new File_ObjectInfo());
            fobs->fileptr = nullptr;
            fobs->obj_infos.reserve(OBJECTINFO_SIZE);
            for (auto& btmp : bitRanges) {
                ObjectInfo tinfo;
                tinfo.offset = btmp.beginIndex << PAGE_SIZE_SHIFT;
                tinfo.length = (btmp.endIndex - btmp.beginIndex + 1) << PAGE_SIZE_SHIFT;
                tinfo.sn = 0;
                tinfo.snapptr = nullptr;
                fobs->obj_infos.push_back(tinfo);
            }

            file_objs.push_back(std::move(fobs));

            return;
        }

    } else {
        while (true != isFinish) {
            std::unique_ptr<File_ObjectInfo> fobs(new File_ObjectInfo());
            fobs->obj_infos.reserve(OBJECTINFO_SIZE);
            isFinish = cloneFile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, fobs->obj_infos);
            if (true != fobs->obj_infos.empty()) {
                fobs->fileptr = cloneFile;
                file_objs.push_back (std::move(fobs));
            }

            if (true == isFinish) { //all the objInfos is in the map
                return;
            }

            //initialize the bitranges and notInMapBitRanges
            bitRanges = notInMapBitRanges;
            notInMapBitRanges.clear();

            cloneFile = nullptr;
            struct CloneInfos tmpclone;
            while (nullptr == cloneFile) {
                tmpclone = getParentClone (ctx->clones, cloneNo);
                cloneParentNo = tmpclone.cloneNo;
                cloneSn = tmpclone.cloneSn;
                cloneNo = cloneParentNo;
                if (0 == cloneParentNo) {
                    break;
                }
                cloneFile = cloneCache_.Get(ctx->rootId, cloneParentNo);
            }

            if (nullptr == cloneFile) { //must be zero, not any clone chunk left, just search the chunkfile
                assert (0 == cloneParentNo);
                if (rootChunkFile != nullptr) {
                    std::unique_ptr<File_ObjectInfo> fobsi(new File_ObjectInfo());
                    fobsi->obj_infos.reserve(OBJECTINFO_SIZE);
                    isFinish = rootChunkFile->DivideObjInfoByIndex (cloneSn, bitRanges, notInMapBitRanges, fobsi->obj_infos);
                    if (true != fobsi->obj_infos.empty()) {
                        fobsi->fileptr = rootChunkFile;
                        file_objs.push_back (std::move(fobsi));
                    }
                    assert (isFinish == true);

                    return;
                } else { //not any clonefile and root file exists, just fill with zero
                    std::unique_ptr<File_ObjectInfo> fobsi(new File_ObjectInfo());
                    fobsi->obj_infos.reserve(OBJECTINFO_SIZE);
                    fobsi->fileptr = nullptr;
                    for (auto& btmp : bitRanges) {
                        ObjectInfo tinfo;
                        tinfo.offset = btmp.beginIndex << PAGE_SIZE_SHIFT;
                        tinfo.length = (btmp.endIndex - btmp.beginIndex + 1) << PAGE_SIZE_SHIFT;
                        tinfo.sn = 0;
                        tinfo.snapptr = nullptr;
                        fobsi->obj_infos.push_back(tinfo);
                    }

                    file_objs.push_back(std::move(fobsi));
                }
            }
        }
    }

    return;
}

//func which help to read from objInfo
CSErrorCode CSDataStore::ReadByObjInfo (CSChunkFilePtr fileptr, char* buf, ObjectInfo& objInfo) {
    CSErrorCode errorCode;

    if (nullptr == fileptr) {//should memset with 0, or with never mind? root chunk does not exist
        memset(buf, 0, objInfo.length);
    } else if ((nullptr == objInfo.snapptr) && (0 == objInfo.sn)) {
        std::cout << "read direct chunk file " << std::endl;
        errorCode = fileptr->Read (buf, objInfo.offset, objInfo.length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo.sn;
            return errorCode;
        }
    } else if ((nullptr == objInfo.snapptr) && (0 != objInfo.sn)) {
        errorCode = fileptr->ReadSpecifiedChunk (objInfo.sn, buf, objInfo.offset, objInfo.length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo.sn;
            return errorCode;
        }
    } else {
        errorCode = fileptr->ReadSpecifiedSnap (objInfo.sn, objInfo.snapptr, buf, objInfo.offset, objInfo.length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                         << "ReadByObjInfo read sn = " << objInfo.sn;
            return errorCode;
        }
    }

    return CSErrorCode::Success;
}

/*
    build obj vector for the specified  offset and length 
    according to the OBJ_SIZE to split the offset and length into several parts
    asume that the clone chunk use the object unit is OBJ_SIZE which is multiple of page size
    and use the OBJ_SIZE to split the offset and length into several parts
    and to check if the parts is in the chunk by bitmap
    the default OBJ_SIZE is 64KB
*/
void CSDataStore::SplitDataIntoObjs (SequenceNum sn,
                                    std::vector<File_ObjectInfoPtr>& objInfos, 
                                    off_t offset, size_t length,
                                    std::unique_ptr<CloneContext>& ctx) {
    //if the offset is align with OBJ_SIZE then the objNum is length / OBJ_SIZE
    //else the objNum is length / OBJ_SIZE + 1

    uint32_t beginIndex = offset >> PAGE_SIZE_SHIFT;    
    uint32_t endIndex = (offset + length - 1) >> PAGE_SIZE_SHIFT;
    
    std::cout << "CSDataStore::SplitDataIntoObjs " << " beginIndex = " << beginIndex << " endIndex = " << endIndex << std::endl;

    searchChunkForObj (sn, objInfos, beginIndex, endIndex, ctx);
    
    return;
}

//another ReadChunk Interface for the clone chunk
CSErrorCode CSDataStore::ReadChunk(ChunkID id,
                                   SequenceNum sn,
                                   char * buf,
                                   off_t offset,
                                   size_t length,
                                   std::unique_ptr<CloneContext>& ctx) {
    
    CSChunkFilePtr chunkFile = nullptr;
    //if it is clone chunk, means tha the chunkid is the root of clone chunk
    //so we need to use the vector clone to get the parent clone chunk
    if (ctx->cloneNo > 0) { 

        std::vector<File_ObjectInfoPtr> objInfos;
        SplitDataIntoObjs (sn, objInfos, offset, length, ctx);

        CSErrorCode errorCode;
        for (auto& fileobj: objInfos) {
            for (auto& objInfo: fileobj->obj_infos) {
                errorCode = ReadByObjInfo (fileobj->fileptr, buf + (objInfo.offset - offset), objInfo);
                if (errorCode != CSErrorCode::Success) {
                    LOG(WARNING) << "Read chunk file failed."
                                << "ChunkID = " << id;
                    return errorCode;
                }
            }
        }
    } else {
        chunkFile = metaCache_.Get(id);
        if (chunkFile == nullptr) {
            return CSErrorCode::ChunkNotExistError;
        }

        CSErrorCode errorCode = chunkFile->Read(buf, offset, length);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }        
    }

    return CSErrorCode::Success;
}

//another ReadSnapshotChunk Interface for the clone chunk
CSErrorCode CSDataStore::ReadSnapshotChunk(ChunkID id,
                                           SequenceNum sn,
                                           char * buf,
                                           off_t offset,
                                           size_t length,
                                           std::shared_ptr<SnapContext> ctx,
                                           std::unique_ptr<CloneContext>& cloneCtx) {

    if (ctx != nullptr && !ctx->contains(sn)) {
        return CSErrorCode::SnapshotNotExistError;
    }

    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }

    //if the chunkfile exist and it is not a clone chunk
    if ((nullptr != chunkFile) && (0 == cloneCtx->cloneNo)) {
        CSErrorCode errorCode =
            chunkFile->ReadSpecifiedChunk(sn, buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Read snapshot chunk failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    } else {
        std::vector<File_ObjectInfoPtr> objInfos;
        SplitDataIntoObjs (sn, objInfos, offset, length, cloneCtx);

        CSErrorCode errorCode;
        for (auto& fileobj : objInfos) {
            for (auto& objInfo: fileobj->obj_infos) {
                errorCode = ReadByObjInfo (fileobj->fileptr, buf + (objInfo.offset - offset), objInfo);
                if (errorCode != CSErrorCode::Success) {
                    LOG(WARNING) << "Read chunk file failed."
                                << "ChunkID = " << id;
                    return errorCode;
                }
            }
        }
    }

    return CSErrorCode::Success;
}

// It is ensured that if snap chunk exists, the chunk must exist.
// 1. snap chunk is generated from COW, thus chunk must exist.
// 2. discard will not delete chunk if there is snapshot.
CSErrorCode CSDataStore::ReadSnapshotChunk(ChunkID id,
                                           SequenceNum sn,
                                           char * buf,
                                           off_t offset,
                                           size_t length,
                                           std::shared_ptr<SnapContext> ctx) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        return CSErrorCode::ChunkNotExistError;
    }
    if (ctx != nullptr && !ctx->contains(sn)) {
        return CSErrorCode::SnapshotNotExistError;
    }
    CSErrorCode errorCode =
        chunkFile->ReadSpecifiedChunk(sn, buf, offset, length);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Read snapshot chunk failed."
                     << "ChunkID = " << id;
    }
    return errorCode;
}

CSChunkFilePtr CSDataStore::GetChunkFile (ChunkID id) {
    return metaCache_.Get(id);
}

CSErrorCode CSDataStore::CreateChunkFile(const ChunkOptions & options,
                                         CSChunkFilePtr* chunkFile) {
        if (!options.location.empty() &&
            options.location.size() > locationLimit_) {
            LOG(ERROR) << "Location is too long."
                       << "ChunkID = " << options.id
                       << ", location = " << options.location
                       << ", location size = " << options.location.size()
                       << ", location limit size = " << locationLimit_;
            return CSErrorCode::InvalidArgError;
        }
        auto tempChunkFile = std::make_shared<CSChunkFile>(lfs_,
                                                  chunkFilePool_,
                                                  options);
        CSErrorCode errorCode = tempChunkFile->Open(true);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Create chunk file failed."
                         << "ChunkID = " << options.id
                         << ", ErrorCode = " << errorCode;
            return errorCode;
        }
        // If there are two operations concurrently to create a chunk file,
        // Then the chunkFile generated by one of the operations will be added
        // to metaCache first, the subsequent operation abandons the currently
        // generated chunkFile and uses the previously generated chunkFile
        *chunkFile = metaCache_.Set(options.id, tempChunkFile);

        if (options.cloneNo > 0) {
            cloneCache_.Set(options.rootId, options.cloneNo, tempChunkFile);
        }

        return CSErrorCode::Success;
}

static void CloneBufferDeleter(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

int CSDataStore::FindExtraReadFromParent(std::vector<File_ObjectInfoPtr>& objIns, CSChunkFilePtr& chunkFile, off_t offset, size_t length) {

    int buf_size = 0;

    for (auto& tmpo : objIns) {
        if (tmpo->fileptr != chunkFile) {//for any data that not in chunk itself
            for (auto iter = tmpo->obj_infos.begin(); iter != tmpo->obj_infos.end();) {
                struct ObjectInfo& tmpi = *iter;
                if (tmpi.offset < offset) {
                    if ((tmpi.offset + tmpi.length >= offset) && (tmpi.offset + tmpi.length <= offset + length)) {
                        tmpi.length = offset - tmpi.offset;
                        buf_size += tmpi.length;
                        iter++;
                    } else if ((tmpi.offset + tmpi.length >= offset) && (tmpi.offset + tmpi.length > offset + length)) {
                        int tlength = tmpi.length;
                        tmpi.length = offset - tmpi.offset;
                        buf_size += tmpi.length;

                        struct ObjectInfo tmpj = tmpi;
                        tmpj.offset = offset + length;
                        tmpj.length = tmpi.offset + tlength - tmpj.offset;
                        iter = tmpo->obj_infos.insert(iter + 1, tmpj);
                        buf_size += tmpj.length;
                        iter++;
                    } else {
                        buf_size += tmpi.length;
                        iter++;
                    }
                } else if (tmpi.offset == offset) {
                    if (tmpi.offset + tmpi.length <= offset + length) {
                        iter = tmpo->obj_infos.erase(iter);
                    } else {
                        int toff = tmpi.offset;
                        int tlen = tmpi.length;
                        tmpi.offset = offset + length;
                        tmpi.length = toff + tlen - tmpi.offset;
                        buf_size += tmpi.length;
                        iter++;
                    }
                } else {
                    if (tmpi.offset + tmpi.length <= offset + length) {
                        iter = tmpo->obj_infos.erase(iter);
                    } else if (tmpi.offset >= offset + length){
                        buf_size += tmpi.length;
                        iter++;
                    } else {
                        int toff = tmpi.offset;
                        int tlen = tmpi.length;
                        tmpi.offset = offset + length;
                        tmpi.length = toff + tlen - tmpi.offset;
                        buf_size += tmpi.length;
                        iter++;
                    }
                }
            }
        }
    }

    return buf_size;

}

void CSDataStore::MergeObjectForRead(std::map<int32_t, Offset_InfoPtr>& objmap, 
                                     std::vector<File_ObjectInfoPtr>& objIns, 
                                     CSChunkFilePtr& chunkFile) {

    for (auto& tmpo : objIns) {
        if (tmpo->fileptr != chunkFile) {
            for (auto& tmpi : tmpo->obj_infos) {
                auto it_upper = objmap.upper_bound(tmpi.offset);
                auto it_lower = it_upper;
                if (it_lower != objmap.begin()) {
                    it_lower--;
                } else {
                    it_lower = objmap.end();
                }

                if (it_lower != objmap.end()) {//find smaller offset
                    if (it_lower->second->offset + it_lower->second->length == tmpi.offset) {
                        struct File_Object fobj(tmpo->fileptr, tmpi);

                        it_lower->second->length = tmpi.length + it_lower->second->length;
                        it_lower->second->objs.push_back(fobj);

                        if (it_upper != objmap.end()) {
                            if (it_lower->second->offset + it_lower->second->length == it_upper->second->offset) {
                                for (auto& tmp : it_upper->second->objs) {
                                    it_lower->second->objs.push_back(tmp);
                                }

                                it_lower->second->length = it_upper->second->length + it_lower->second->length;
                                objmap.erase(it_upper);                                    
                            }
                        }
                    } else {
                        if (it_upper != objmap.end()) {//find bigger offset
                            if (tmpi.offset + tmpi.length == it_upper->second->offset) {
                                Offset_InfoPtr tptr = std::move(it_upper->second);
                                struct File_Object fobj(tmpo->fileptr, tmpi);
                                tptr->objs.push_back(fobj);
                                tptr->offset = tmpi.offset;
                                tptr->length = tmpi.length + tptr->length;
                                objmap.erase(it_upper);
                                objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(tptr)));                                    
                            } else {
                                struct File_Object fobj(tmpo->fileptr, tmpi);

                                Offset_InfoPtr infoptr(new Offset_Info());
                                infoptr->offset = tmpi.offset;
                                infoptr->length = tmpi.length;
                                infoptr->objs.push_back(fobj);
                                objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(infoptr))); 
                            }
                        } else {
                            struct File_Object fobj(tmpo->fileptr, tmpi);

                            Offset_InfoPtr infoptr(new Offset_Info());
                            infoptr->offset = tmpi.offset;
                            infoptr->length = tmpi.length;
                            infoptr->objs.push_back(fobj);
                            objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(infoptr))); 
                        }
                    }
                } else {
                    if (it_upper != objmap.end()) { //find bigger offset
                        if (tmpi.offset + tmpi.length == it_upper->second->offset) {//merge the objs
                            Offset_InfoPtr tptr = std::move(it_upper->second);
                            struct File_Object fobj(tmpo->fileptr, tmpi);
                            tptr->objs.push_back(fobj);
                            tptr->offset = tmpi.offset;
                            tptr->length = tmpi.length + tptr->length;
                            objmap.erase(it_upper);
                            objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(tptr)));
                        } else {
                            struct File_Object fobj(tmpo->fileptr, tmpi);

                            Offset_InfoPtr infoptr(new Offset_Info());
                            infoptr->offset = tmpi.offset;
                            infoptr->length = tmpi.length;
                            infoptr->objs.push_back(fobj);
                            objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(infoptr))); 
                        }
                    } else {
                        struct File_Object fobj(tmpo->fileptr, tmpi);

                        Offset_InfoPtr infoptr(new Offset_Info());
                        infoptr->offset = tmpi.offset;
                        infoptr->length = tmpi.length;
                        infoptr->objs.push_back(fobj);
                        objmap.insert(std::pair<uint32_t, Offset_InfoPtr>(tmpi.offset, std::move(infoptr))); 
                    }
                }
            }
        }
        
    }


    return;
}

//WriteChunk interface for the clone chunk
CSErrorCode CSDataStore::WriteChunk (ChunkID id, SequenceNum sn,
                                    const butil::IOBuf& buf, off_t offset, size_t length,
                                    uint32_t* cost, std::shared_ptr<SnapContext> ctx, 
                                    std::unique_ptr<CloneContext>& cloneCtx) {
    
    CSErrorCode errorCode = CSErrorCode::Success;

    // The requested sequence number is not allowed to be 0, when snapsn=0,
    // it will be used as the basis for judging that the snapshot does not exist
    if (sn == kInvalidSeq) {
        LOG(ERROR) << "Sequence num should not be zero."
                   << "ChunkID = " << id;
        return CSErrorCode::InvalidArgError;
    }

    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        options.cloneNo = cloneCtx->cloneNo;
        options.rootId = cloneCtx->rootId;
        options.enableOdsyncWhenOpenChunkFile = enableOdsyncWhenOpenChunkFile_;
        errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }

    //if it is clone chunk
    if (0 == cloneCtx->cloneNo) {//not clone chunk
        // write chunk file
        errorCode = chunkFile->Write(sn, buf, offset, length, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }

        return errorCode;
    } 
    
    //asume that the clone chunk use the object unit is OBJ_SIZE which is multiple of page size
    //and use the OBJ_SIZE to split the offset and length into several parts
    //and to check if the parts is in the chunk by bitmap
    //the default OBJ_SIZE is 64KB

    //if the offset is align with OBJ_SIZE then the objNum is length / OBJ_SIZE
    //else the objNum is length / OBJ_SIZE + 1
    uint32_t beginIndex = offset >> OBJ_SIZE_SHIFT;
    uint32_t endIndex = (offset + length - 1) >> OBJ_SIZE_SHIFT;

    //Judge that the clonefile has the data in <beginIndex, endIndex>
    //if exist just pass to the next step
    //if not read the uncover data and 1. write to the clone file  2. write to snapshot
    std::vector<File_ObjectInfoPtr> objIns;
    SplitDataIntoObjs (sn, objIns, (beginIndex << OBJ_SIZE_SHIFT), ((endIndex - beginIndex) << OBJ_SIZE_SHIFT), cloneCtx);

    auto objit = objIns.begin();
    File_ObjectInfoPtr& fileobj = *objit;
    if ((1 == objIns.size()) && (fileobj->fileptr == chunkFile)) {//all map is in the chunk
        // write chunk file
        errorCode = chunkFile->Write(sn, buf, offset, length, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }

        return errorCode;
    }

    assert (nullptr != chunkFile); //for clone file the orgin clone must be exists
    assert (chunkFile->getCloneNumber() > 0);

    if (false == chunkFile->need_Cow(sn, ctx)) {//not write to the snapshot but the chunk its self        
        //no need to do read from other clone volume
        if (((endIndex - beginIndex + 1) << OBJ_SIZE_SHIFT) == length) {
            // write chunk file
            errorCode = chunkFile->Write(sn, buf, offset, length, cost, ctx);
            if (errorCode != CSErrorCode::Success) {
                LOG(WARNING) << "Write chunk file failed."
                            << "ChunkID = " << id;
                return errorCode;
            }

            return errorCode;           
        }

        int buf_size = 0;
        buf_size += length;

        buf_size += FindExtraReadFromParent(objIns, chunkFile, offset, length);

        char* tmpbuf = nullptr;
        butil::IOBuf cloneBuf;

        tmpbuf = new char[buf_size];
        uint32_t tmp_offset = beginIndex << OBJ_SIZE_SHIFT;
        for (auto& tmpo : objIns) {
            if (tmpo->fileptr != chunkFile) {//for any data that not in chunk itself
                for (auto& tmpi : tmpo->obj_infos) {
                    errorCode = ReadByObjInfo (tmpo->fileptr, tmpbuf + (tmpi.offset - tmp_offset), tmpi);
                    if (errorCode != CSErrorCode::Success) {
                        free(tmpbuf);
                        LOG(WARNING) << "Write chunk file failed."
                                    << "ChunkID = " << id;
                        return errorCode;
                    }
                }
            }
        }

        buf.copy_to(tmpbuf + (offset - tmp_offset), length);
        cloneBuf.append_user_data(tmpbuf, buf_size, CloneBufferDeleter);

        errorCode = chunkFile->Write(sn, cloneBuf, tmp_offset, buf_size, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }

        return errorCode;

    } else {//snapshot need, so need to do write to the clone chunk which not set
        //first read the data and write data
        int buf_size = 0;
 
        std::map<int32_t, Offset_InfoPtr> objmap;

        MergeObjectForRead(objmap, objIns, chunkFile);

        char* tmpbuf = new char[(endIndex - beginIndex + 1) << OBJ_SIZE_SHIFT];

        for (auto iter = objmap.begin(); iter != objmap.end(); iter++) {
            for (auto& tmpj: iter->second->objs) {
                errorCode = ReadByObjInfo(tmpj.fileptr, tmpbuf + (tmpj.obj.offset - iter->second->offset), tmpj.obj);
                if (errorCode != CSErrorCode::Success) {
                    LOG(WARNING) << "Write chunk file failed."
                                << "ChunkID = " << id;
                    return errorCode;
                }                    
            }

            int rc = chunkFile->writeDataDirect(tmpbuf, iter->second->offset, iter->second->length);
            if (rc != iter->second->length) {
                errorCode = CSErrorCode::InternalError;
                return errorCode;
            }
        }

        delete [] tmpbuf;
        
        errorCode = chunkFile->Write(sn, buf, offset, length, cost, ctx);
        if (errorCode != CSErrorCode::Success) {
            LOG(WARNING) << "Write chunk file failed."
                        << "ChunkID = " << id;
            return errorCode;
        }

        return errorCode;

    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::WriteChunk(ChunkID id,
                            SequenceNum sn,
                            const butil::IOBuf& buf,
                            off_t offset,
                            size_t length,
                            uint32_t* cost,
                            std::shared_ptr<SnapContext> ctx,
                            const std::string & cloneSourceLocation)  {
    // The requested sequence number is not allowed to be 0, when snapsn=0,
    // it will be used as the basis for judging that the snapshot does not exist
    if (sn == kInvalidSeq) {
        LOG(ERROR) << "Sequence num should not be zero."
                   << "ChunkID = " << id;
        return CSErrorCode::InvalidArgError;
    }
    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.location = cloneSourceLocation;
        options.pageSize = pageSize_;
        options.metric = metric_;
        options.enableOdsyncWhenOpenChunkFile = enableOdsyncWhenOpenChunkFile_;
        CSErrorCode errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }
    // write chunk file
    CSErrorCode errorCode = chunkFile->Write(sn,
                                             buf,
                                             offset,
                                             length,
                                             cost,
                                             ctx);
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Write chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::SyncChunk(ChunkID id) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(WARNING) << "Sync chunk not exist, ChunkID = " << id;
        return CSErrorCode::Success;
    }
    CSErrorCode errorCode = chunkFile->Sync();
    if (errorCode != CSErrorCode::Success) {
        LOG(WARNING) << "Sync chunk file failed."
                     << "ChunkID = " << id;
        return errorCode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::CreateCloneChunk(ChunkID id,
                                          SequenceNum sn,
                                          SequenceNum correctedSn,
                                          ChunkSizeType size,
                                          const string& location) {
    // Check the validity of the parameters
    if (size != chunkSize_
        || sn == kInvalidSeq
        || location.empty()) {
        LOG(ERROR) << "Invalid arguments."
                   << "ChunkID = " << id
                   << ", sn = " << sn
                   << ", correctedSn = " << correctedSn
                   << ", size = " << size
                   << ", location = " << location;
        return CSErrorCode::InvalidArgError;
    }
    auto chunkFile = metaCache_.Get(id);
    // If the chunk file does not exist, create the chunk file first
    if (chunkFile == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = sn;
        options.correctedSn = correctedSn;
        options.location = location;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSErrorCode errorCode = CreateChunkFile(options, &chunkFile);
        if (errorCode != CSErrorCode::Success) {
            return errorCode;
        }
    }
    // Determine whether the specified parameters match the information
    // in the existing Chunk
    // No need to put in else, because users may call this interface at the
    // same time
    // If different sequence or location information are specified in the
    // parameters, there may be concurrent conflicts, and judgments are also
    // required
    CSChunkInfo info;
    chunkFile->GetInfo(&info);
    if (info.location.compare(location) != 0
        || info.curSn != sn
        || info.correctedSn != correctedSn) {
        LOG(WARNING) << "Conflict chunk already exists."
                   << "sn in arg = " << sn
                   << ", correctedSn in arg = " << correctedSn
                   << ", location in arg = " << location
                   << ", sn in chunk = " << info.curSn
                   << ", location in chunk = " << info.location
                   << ", corrected sn in chunk = " << info.correctedSn;
        return CSErrorCode::ChunkConflictError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::PasteChunk(ChunkID id,
                                    const char * buf,
                                    off_t offset,
                                    size_t length) {
    auto chunkFile = metaCache_.Get(id);
    // Paste Chunk requires Chunk must exist
    if (chunkFile == nullptr) {
        LOG(WARNING) << "Paste Chunk failed, Chunk not exists."
                     << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    CSErrorCode errcode = chunkFile->Paste(buf, offset, length);
    if (errcode != CSErrorCode::Success) {
        LOG(WARNING) << "Paste Chunk failed, Chunk not exists."
                     << "ChunkID = " << id;
        return errcode;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::GetChunkInfo(ChunkID id,
                                      CSChunkInfo* chunkInfo) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(INFO) << "Get ChunkInfo failed, Chunk not exists."
                  << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    chunkFile->GetInfo(chunkInfo);
    return CSErrorCode::Success;
}

CSErrorCode CSDataStore::GetChunkHash(ChunkID id,
                                      off_t offset,
                                      size_t length,
                                      std::string* hash) {
    auto chunkFile = metaCache_.Get(id);
    if (chunkFile == nullptr) {
        LOG(INFO) << "Get ChunkHash failed, Chunk not exists."
                  << "ChunkID = " << id;
        return CSErrorCode::ChunkNotExistError;
    }
    return chunkFile->GetHash(offset, length, hash);
}

DataStoreStatus CSDataStore::GetStatus() {
    DataStoreStatus status;
    status.chunkFileCount = metric_->chunkFileCount.get_value();
    status.cloneChunkCount = metric_->cloneChunkCount.get_value();
    status.snapshotCount = metric_->snapshotCount.get_value();
    return status;
}

CSErrorCode CSDataStore::loadChunkFile(ChunkID id) {
    // If the chunk file has not been loaded yet, load it into metaCache
    if (metaCache_.Get(id) == nullptr) {
        ChunkOptions options;
        options.id = id;
        options.sn = 0;
        options.baseDir = baseDir_;
        options.chunkSize = chunkSize_;
        options.pageSize = pageSize_;
        options.metric = metric_;
        CSChunkFilePtr chunkFilePtr =
            std::make_shared<CSChunkFile>(lfs_,
                                          chunkFilePool_,
                                          options);
        CSErrorCode errorCode = chunkFilePtr->Open(false);
        if (errorCode != CSErrorCode::Success)
            return errorCode;
        metaCache_.Set(id, chunkFilePtr);

        uint64_t cloneno = chunkFilePtr->getCloneNumber();
        if (cloneno > 0) {
            auto tmpptr = cloneCache_.Set(id, cloneno, chunkFilePtr);
            assert (tmpptr == chunkFilePtr);
        }
    }
    return CSErrorCode::Success;
}

ChunkMap CSDataStore::GetChunkMap() {
    return metaCache_.GetMap();
}

SnapContext::SnapContext(const std::vector<SequenceNum>& snapIds) {
    std::copy(snapIds.begin(), snapIds.end(), std::back_inserter(snaps));
}

SequenceNum SnapContext::getPrev(SequenceNum snapSn) const {
    SequenceNum n = 0;
    for (long i = 0; i < snaps.size(); i++) {
        if (snaps[i] >= snapSn) {
            break;
        }
        n = snaps[i];
    }

    return n;
}

SequenceNum SnapContext::getNext(SequenceNum snapSn) const {
    auto it = std::find_if(snaps.begin(), snaps.end(), [&](SequenceNum n) {return n > snapSn;});
    return it == snaps.end() ? 0 : *it;
}

SequenceNum SnapContext::getLatest() const {
    return snaps.empty() ? 0 : *snaps.rbegin();
}

bool SnapContext::contains(SequenceNum snapSn) const {
    return std::find(snaps.begin(), snaps.end(), snapSn) != snaps.end();
}

bool SnapContext::empty() const {
    return snaps.empty();
}

}  // namespace chunkserver
}  // namespace curve
