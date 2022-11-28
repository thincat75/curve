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
 * Created Date: 18-10-11
 * Author: wudemiao
 */

#include <gtest/gtest.h>

#include "src/client/request_sender_manager.h"
#include "src/client/client_common.h"

namespace curve {
namespace client {

TEST(RequestSenderManagerTest, basic_test) {
    IOSenderOption ioSenderOpt;
    ioSenderOpt.failRequestOpt.chunkserverOPMaxRetry = 3;
    ioSenderOpt.failRequestOpt.chunkserverOPRetryIntervalUS = 500;
    ioSenderOpt.chunkserverEnableAppliedIndexRead = 1;

    std::unique_ptr<RequestSenderManager> senderManager(
        new RequestSenderManager());
    ChunkServerID leaderId = 123456789;
    butil::EndPoint leaderAddr;
    std::string leaderStr = "127.0.0.1:9109";
    butil::str2endpoint(leaderStr.c_str(), &leaderAddr);

    for (int i = leaderId; i <= leaderId + 10000; ++i) {
        auto senderPtr1 = senderManager->GetOrCreateSender(leaderId,
                                                           leaderAddr,
                                                           ioSenderOpt);
        ASSERT_TRUE(nullptr != senderPtr1);
    }
}

// TEST(RequestSenderManagerTest, fail_test) {
//     IOSenderOption ioSenderOpt;
//     ioSenderOpt.failRequestOpt.chunkserverOPMaxRetry = 3;
//     ioSenderOpt.failRequestOpt.chunkserverOPRetryIntervalUS = 500;
//     ioSenderOpt.chunkserverEnableAppliedIndexRead = 1;

//     std::unique_ptr<RequestSenderManager> senderManager(
//         new RequestSenderManager());
//     ChunkServerID leaderId = 123456789;
//     butil::EndPoint leaderAddr;
//     leaderAddr.ip = {0U};
//     leaderAddr.port = -1;

//     ASSERT_EQ(nullptr, senderManager->GetOrCreateSender(
//         leaderId, leaderAddr, ioSenderOpt));
// }

TEST(RequestSenderManagerTest, UcpTest) {
    IOSenderOption ioSenderOpt;
    ioSenderOpt.failRequestOpt.chunkserverOPMaxRetry = 3;
    ioSenderOpt.failRequestOpt.chunkserverOPRetryIntervalUS = 500;
    ioSenderOpt.chunkserverEnableAppliedIndexRead = 1;

    RequestSenderManager senderManager;
    ChunkServerID leaderId = 1;
    butil::EndPoint leaderAddr;
    ASSERT_EQ(0, butil::str2endpoint("127.0.0.1:8200", &leaderAddr));

    auto tcpSender =
            senderManager.GetOrCreateSender(leaderId, leaderAddr, ioSenderOpt);
    ASSERT_NE(nullptr, tcpSender);

    butil::EndPoint leaderAddrUcp = leaderAddr;
    leaderAddrUcp.set_ucp();

    auto ucpSender = senderManager.GetOrCreateSender(leaderId, leaderAddrUcp,
                                                     ioSenderOpt);
    ASSERT_NE(nullptr, ucpSender);
    ASSERT_NE(tcpSender.get(), ucpSender.get());
}   

}   // namespace client
}   // namespace curve
