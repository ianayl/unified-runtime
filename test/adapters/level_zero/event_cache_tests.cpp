// Copyright (C) 2024 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ur_print.hpp"
#include "uur/fixtures.h"
#include "uur/raii.h"

#include <map>
#include <string>

template <typename... Args> auto combineFlags(std::tuple<Args...> tuple) {
    return std::apply([](auto... args) { return (... |= args); }, tuple);
}

extern std::map<std::string, int> *ZeCallCount;

using FlagsTupleType = std::tuple<ur_queue_flags_t, ur_queue_flags_t,
                                  ur_queue_flags_t, ur_queue_flags_t>;

struct urEventCacheTest : uur::urContextTestWithParam<FlagsTupleType> {
    void SetUp() override {
        UUR_RETURN_ON_FATAL_FAILURE(urContextTestWithParam::SetUp());

        flags = combineFlags(getParam());

        ur_queue_properties_t props;
        props.flags = flags;
        ASSERT_SUCCESS(urQueueCreate(context, device, &props, &queue));
        ASSERT_NE(queue, nullptr);

        ASSERT_SUCCESS(urMemBufferCreate(context, UR_MEM_FLAG_WRITE_ONLY, size,
                                         nullptr, &buffer));

        (*ZeCallCount)["zeEventCreate"] = 0;
        (*ZeCallCount)["zeEventDestroy"] = 0;
    }

    void TearDown() override {
        if (buffer) {
            EXPECT_SUCCESS(urMemRelease(buffer));
        }
        if (queue) {
            UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urQueueRelease(queue));
        }
        UUR_RETURN_ON_FATAL_FAILURE(urContextTestWithParam::TearDown());
    }

    auto enqueueWork(ur_event_handle_t *hEvent, int data) {
        input.assign(count, data);
        UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urEnqueueMemBufferWrite(
            queue, buffer, false, 0, size, input.data(), 0, nullptr, hEvent));
    }

    void verifyData() {
        std::vector<uint32_t> output(count, 1);
        UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urEnqueueMemBufferRead(
            queue, buffer, true, 0, size, output.data(), 0, nullptr, nullptr));

        if (!(flags & UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE)) {
            ASSERT_EQ(input, output);
        }
    }

    const size_t count = 1024;
    const size_t size = sizeof(uint32_t) * count;
    ur_mem_handle_t buffer = nullptr;
    ur_queue_handle_t queue = nullptr;
    std::vector<uint32_t> input;
    ur_queue_flags_t flags;
};

TEST_P(urEventCacheTest, eventsReuseNoVisibleEvent) {
    static constexpr int numIters = 16;
    static constexpr int numEnqueues = 128;

    for (int i = 0; i < numIters; i++) {
        for (int j = 0; j < numEnqueues; j++) {
            enqueueWork(nullptr, i * numEnqueues + j);
        }
        UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urQueueFinish(queue));
        verifyData();
    }

    // TODO: why events are not reused for UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE?
    if ((flags & UR_QUEUE_FLAG_DISCARD_EVENTS) &&
        !(flags & UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE)) {
        ASSERT_EQ((*ZeCallCount)["zeEventCreate"], 2);
    } else {
        ASSERT_GE((*ZeCallCount)["zeEventCreate"], numIters * numEnqueues);
    }
}

TEST_P(urEventCacheTest, eventsReuseWithVisibleEvent) {
    static constexpr int numIters = 16;
    static constexpr int numEnqueues = 128;

    for (int i = 0; i < numIters; i++) {
        std::vector<uur::raii::Event> events(numEnqueues);
        for (int j = 0; j < numEnqueues; j++) {
            enqueueWork(events[j].ptr(), i * numEnqueues + j);
        }
        UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urQueueFinish(queue));
        verifyData();
    }

    ASSERT_LT((*ZeCallCount)["zeEventCreate"], numIters * numEnqueues);
}

TEST_P(urEventCacheTest, eventsReuseWithVisibleEventAndWait) {
    static constexpr int numIters = 16;
    static constexpr int numEnqueues = 128;
    static constexpr int waitEveryN = 16;

    for (int i = 0; i < numIters; i++) {
        std::vector<uur::raii::Event> events;
        for (int j = 0; j < numEnqueues; j++) {
            events.emplace_back();
            enqueueWork(events.back().ptr(), i * numEnqueues + j);

            if (j > 0 && j % waitEveryN == 0) {
                ASSERT_SUCCESS(urEventWait(waitEveryN,
                                           (ur_event_handle_t *)events.data()));
                verifyData();
                events.clear();
            }
        }
        UUR_ASSERT_SUCCESS_OR_UNSUPPORTED(urQueueFinish(queue));
    }

    ASSERT_GE((*ZeCallCount)["zeEventCreate"], waitEveryN);
    // TODO: why there are more events than this?
    // ASSERT_LE((*ZeCallCount)["zeEventCreate"],  waitEveryN * 2 + 2);
}

template <typename T>
inline std::string
printFlags(const testing::TestParamInfo<typename T::ParamType> &info) {
    const auto device_handle = std::get<0>(info.param);
    const auto platform_device_name =
        uur::GetPlatformAndDeviceName(device_handle);
    auto flags = combineFlags(std::get<1>(info.param));

    std::stringstream ss;
    ur::details::printFlag<ur_queue_flag_t>(ss, flags);

    auto str = ss.str();
    std::replace(str.begin(), str.end(), ' ', '_');
    std::replace(str.begin(), str.end(), '|', '_');
    return platform_device_name + "__" + str;
}

UUR_TEST_SUITE_P(
    urEventCacheTest,
    ::testing::Combine(
        testing::Values(0, UR_QUEUE_FLAG_DISCARD_EVENTS),
        testing::Values(0, UR_QUEUE_FLAG_OUT_OF_ORDER_EXEC_MODE_ENABLE),
        // TODO: why the test fails with UR_QUEUE_FLAG_SUBMISSION_BATCHED?
        testing::Values(
            UR_QUEUE_FLAG_SUBMISSION_IMMEDIATE /*, UR_QUEUE_FLAG_SUBMISSION_BATCHED */),
        testing::Values(0, UR_QUEUE_FLAG_PROFILING_ENABLE)),
    printFlags<urEventCacheTest>);
