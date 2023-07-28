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
 * Created Date: Fri Aug 30 2019
 * Author: xuchaojie
 */

#ifndef SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVERCLIENT_CONFIG_H_
#define SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVERCLIENT_CONFIG_H_

#include <string>

namespace curve {
namespace mds {
namespace chunkserverclient {

struct ChunkServerClientOption {
    uint32_t rpcTimeoutMs;
    uint32_t rpcRetryTimes;
    uint32_t rpcRetryIntervalMs;
    uint32_t rpcMaxTimeoutMs;
    uint32_t updateLeaderRetryTimes;
    uint32_t updateLeaderRetryIntervalMs;
    ChunkServerClientOption()
        : rpcTimeoutMs(500),
          rpcRetryTimes(10),
          rpcRetryIntervalMs(500),
          rpcMaxTimeoutMs(8000),
          updateLeaderRetryTimes(3),
          updateLeaderRetryIntervalMs(5000) {}
};

}  // namespace chunkserverclient
}  // namespace mds
}  // namespace curve

#endif  // SRC_MDS_CHUNKSERVERCLIENT_CHUNKSERVERCLIENT_CONFIG_H_
