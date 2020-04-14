/*
 * Project: curve
 * File Created: Monday, 17th September 2018 3:26:18 pm
 * Author: tongguangxun
 * Copyright (c) 2018 NetEase
 */

#include <glog/logging.h>

#include <algorithm>

#include "src/client/splitor.h"
#include "src/client/iomanager.h"
#include "src/client/io_tracker.h"
#include "src/client/request_scheduler.h"
#include "src/client/request_closure.h"
#include "src/common/timeutility.h"

using curve::chunkserver::CHUNK_OP_STATUS;

namespace curve {
namespace client {

std::atomic<uint64_t> IOTracker::tracekerID_(1);

IOTracker::IOTracker(IOManager* iomanager,
                        MetaCache* mc,
                        RequestScheduler* scheduler,
                        FileMetric_t* clientMetric):
                        mc_(mc),
                        iomanager_(iomanager),
                        scheduler_(scheduler),
                        fileMetric_(clientMetric) {
    id_         = tracekerID_.fetch_add(1);
    scc_        = nullptr;
    aioctx_     = nullptr;
    data_       = nullptr;
    type_       = OpType::UNKNOWN;
    errcode_    = LIBCURVE_ERROR::OK;
    offset_     = 0;
    length_     = 0;
    reqlist_.clear();
    reqcount_.store(0, std::memory_order_release);
    opStartTimePoint_ = curve::common::TimeUtility::GetTimeofDayUs();
}

void IOTracker::StartRead(CurveAioContext* aioctx, char* buf,
    off_t offset, size_t length, MDSClient* mdsclient, const FInfo_t* fi) {
    data_   = buf;
    offset_ = offset;
    length_ = length;
    aioctx_ = aioctx;
    type_   = OpType::READ;

    DVLOG(9)  << "read op, offset = " << offset
              << ", length = " << length;

    int ret = Splitor::IO2ChunkRequests(this, mc_, &reqlist_, data_,
                                        offset_, length_, mdsclient, fi);
    if (ret == 0) {
        reqcount_.store(reqlist_.size(), std::memory_order_release);
        std::for_each(reqlist_.begin(), reqlist_.end(), [&](RequestContext* r) {
            r->done_->SetFileMetric(fileMetric_);
            r->done_->SetIOManager(iomanager_);
        });
        ret = scheduler_->ScheduleRequest(reqlist_);
    } else {
        LOG(ERROR) << "splitor read io failed, "
                   << "offset = " << offset_
                   << ", length = " << length_;
    }

    if (ret == -1) {
        LOG(ERROR) << "split or schedule failed, return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::StartWrite(CurveAioContext* aioctx, const char* buf,
    off_t offset, size_t length, MDSClient* mdsclient,  const FInfo_t* fi) {
    data_   = buf;
    offset_ = offset;
    length_ = length;
    aioctx_ = aioctx;
    type_   = OpType::WRITE;

    DVLOG(9) << "write op, offset = " << offset
             << ", length = " << length;
    int ret = Splitor::IO2ChunkRequests(this, mc_, &reqlist_, data_, offset_,
                                        length_, mdsclient, fi);
    if (ret == 0) {
        reqcount_.store(reqlist_.size(), std::memory_order_release);
        std::for_each(reqlist_.begin(), reqlist_.end(), [&](RequestContext* r) {
            r->done_->SetFileMetric(fileMetric_);
            r->done_->SetIOManager(iomanager_);
        });
        ret = scheduler_->ScheduleRequest(reqlist_);
    } else {
        LOG(ERROR) << "splitor write io failed, "
                   << "offset = " << offset_
                   << ", length = " << length_;
    }

    if (ret == -1) {
        LOG(ERROR) << "split or schedule failed, return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::ReadSnapChunk(const ChunkIDInfo &cinfo,
    uint64_t seq, uint64_t offset, uint64_t len,
    char *buf, SnapCloneClosure* scc) {
    scc_    = scc;
    data_   = buf;
    offset_ = offset;
    length_ = len;
    type_   = OpType::READ_SNAP;

    int ret = -1;
    do {
        ret = Splitor::SingleChunkIO2ChunkRequests(this, mc_, &reqlist_, cinfo,
                                            data_, offset_, length_, seq);
        if (ret == 0) {
            reqcount_.store(reqlist_.size(), std::memory_order_release);
            ret = scheduler_->ScheduleRequest(reqlist_);
        }
    } while (false);

    if (ret == -1) {
        LOG(ERROR) << "split or schedule failed, return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::DeleteSnapChunkOrCorrectSn(const ChunkIDInfo &cinfo,
    uint64_t correctedSeq) {
    type_ = OpType::DELETE_SNAP;

    int ret = -1;
    do {
        RequestContext* newreqNode = GetInitedRequestContext();
        if (newreqNode == nullptr) {
            break;
        }

        newreqNode->correctedSeq_ = correctedSeq;
        FillCommonFields(cinfo, newreqNode);

        reqlist_.push_back(newreqNode);
        reqcount_.store(reqlist_.size(), std::memory_order_release);

        ret = scheduler_->ScheduleRequest(reqlist_);
    } while (false);

    if (ret == -1) {
        LOG(ERROR) << "DeleteSnapChunkOrCorrectSn request schedule failed,"
                   << "return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::GetChunkInfo(const ChunkIDInfo &cinfo,
    ChunkInfoDetail *chunkInfo) {
    type_ = OpType::GET_CHUNK_INFO;

    int ret = -1;
    do {
        RequestContext* newreqNode = GetInitedRequestContext();
        if (newreqNode == nullptr) {
            break;
        }

        newreqNode->chunkinfodetail_ = chunkInfo;
        FillCommonFields(cinfo, newreqNode);

        reqlist_.push_back(newreqNode);
        reqcount_.store(reqlist_.size(), std::memory_order_release);

        ret = scheduler_->ScheduleRequest(reqlist_);
    } while (false);

    if (ret == -1) {
        LOG(ERROR) << "GetChunkInfo request schedule failed,"
                   << " return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::CreateCloneChunk(const std::string &location,
    const ChunkIDInfo &cinfo, uint64_t sn, uint64_t correntSn,
    uint64_t chunkSize, SnapCloneClosure* scc) {
    type_ = OpType::CREATE_CLONE;
    scc_ = scc;

    int ret = -1;
    do {
        RequestContext* newreqNode = GetInitedRequestContext();
        if (newreqNode == nullptr) {
            break;
        }

        newreqNode->seq_         = sn;
        newreqNode->chunksize_   = chunkSize;
        newreqNode->location_    = location;
        newreqNode->correctedSeq_  = correntSn;
        FillCommonFields(cinfo, newreqNode);

        reqlist_.push_back(newreqNode);
        reqcount_.store(reqlist_.size(), std::memory_order_release);

        ret = scheduler_->ScheduleRequest(reqlist_);
    } while (false);

    if (ret == -1) {
        LOG(ERROR) << "CreateCloneChunk request schedule failed,"
                   << "return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::RecoverChunk(const ChunkIDInfo &cinfo,
    uint64_t offset, uint64_t len, SnapCloneClosure* scc) {
    type_ = OpType::RECOVER_CHUNK;
    scc_  = scc;

    int ret = -1;
    do {
        RequestContext* newreqNode = GetInitedRequestContext();
        if (newreqNode == nullptr) {
            break;
        }

        newreqNode->rawlength_   = len;
        newreqNode->offset_      = offset;
        FillCommonFields(cinfo, newreqNode);

        reqlist_.push_back(newreqNode);
        reqcount_.store(reqlist_.size(), std::memory_order_release);

        ret = scheduler_->ScheduleRequest(reqlist_);
    } while (false);

    if (ret == -1) {
        LOG(ERROR) << "RecoverChunk request schedule failed,"
                   << " return and recyle resource!";
        ReturnOnFail();
    }
}

void IOTracker::FillCommonFields(ChunkIDInfo idinfo, RequestContext* req) {
    req->optype_      = type_;
    req->idinfo_      = idinfo;
    req->done_->SetIOTracker(this);
}

void IOTracker::HandleResponse(RequestContext* reqctx) {
    int errorcode = reqctx->done_->GetErrorCode();
    if (errorcode != 0) {
        ChunkServerErr2LibcurveErr(static_cast<CHUNK_OP_STATUS>(errorcode),
                                   &errcode_);
    }

    if (1 == reqcount_.fetch_sub(1, std::memory_order_acq_rel)) {
        Done();
    }
}

int IOTracker::Wait() {
    return iocv_.Wait();
}

void IOTracker::Done() {
    if (errcode_ == LIBCURVE_ERROR::OK) {
        uint64_t duration = TimeUtility::GetTimeofDayUs() - opStartTimePoint_;
        MetricHelper::UserLatencyRecord(fileMetric_, duration, type_);
        MetricHelper::IncremUserQPSCount(fileMetric_, length_, type_);
    } else {
        MetricHelper::IncremUserEPSCount(fileMetric_, type_);
        if (type_ == OpType::READ || type_ == OpType::WRITE) {
            LOG(ERROR) << "file [" << fileMetric_->filename << "]"
                    << ", IO Error, OpType = " << static_cast<int>(type_)
                    << ", offset = " << offset_
                    << ", length = " << length_;
        } else {
            LOG(ERROR) << ", IO Error, OpType = " << static_cast<int>(type_);
        }
    }

    DestoryRequestList();

    // scc_和aioctx都为空的时候肯定是个同步调用
    if (scc_ == nullptr && aioctx_ == nullptr) {
        errcode_ == LIBCURVE_ERROR::OK ? iocv_.Complete(length_)
                                       : iocv_.Complete(-errcode_);
        return;
    }

    // 异步函数调用，在此处发起回调
    if (aioctx_ != nullptr) {
        aioctx_->ret = errcode_ == LIBCURVE_ERROR::OK ? length_ : -errcode_;
        aioctx_->cb(aioctx_);
    } else {
        int ret = errcode_ == LIBCURVE_ERROR::OK ? length_ : -errcode_;
        scc_->SetRetCode(ret);
        scc_->Run();
    }

    // 回收当前io tracker
    iomanager_->HandleAsyncIOResponse(this);
}

void IOTracker::DestoryRequestList() {
    for (auto iter : reqlist_) {
        iter->UnInit();
        delete iter;
    }
}

void IOTracker::ReturnOnFail() {
    errcode_ = LIBCURVE_ERROR::FAILED;
    Done();
}

void IOTracker::ChunkServerErr2LibcurveErr(CHUNK_OP_STATUS errcode,
    LIBCURVE_ERROR* errout) {
    switch (errcode) {
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_SUCCESS:
            *errout = LIBCURVE_ERROR::OK;
            break;
        // chunk或者copyset对于用户来说是透明的，所以直接返回错误
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_CHUNK_NOTEXIST:
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_COPYSET_NOTEXIST:
            *errout = LIBCURVE_ERROR::NOTEXIST;
            break;
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_CRC_FAIL:
            *errout = LIBCURVE_ERROR::CRC_ERROR;
            break;
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_INVALID_REQUEST:
            *errout = LIBCURVE_ERROR::INVALID_REQUEST;
            break;
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_DISK_FAIL:
            *errout = LIBCURVE_ERROR::DISK_FAIL;
            break;
        case CHUNK_OP_STATUS::CHUNK_OP_STATUS_NOSPACE:
            *errout = LIBCURVE_ERROR::NO_SPACE;
            break;
        default:
            *errout = LIBCURVE_ERROR::FAILED;
            break;
    }
}

RequestContext* IOTracker::GetInitedRequestContext() const {
    RequestContext* reqNode = new (std::nothrow) RequestContext();
    if (reqNode != nullptr && reqNode->Init()) {
        return reqNode;
    } else {
        LOG(ERROR) << "allocate req node failed!";
        delete reqNode;
        return nullptr;
    }
}

}   // namespace client
}   // namespace curve
