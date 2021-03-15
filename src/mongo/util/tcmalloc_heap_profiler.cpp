/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <algorithm>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/static_assert.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/logv2/log.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#include "absl/debugging/symbolize.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"

#include <third_party/murmurhash3/MurmurHash3.h>

#if defined(_POSIX_VERSION)

#include <dlfcn.h>
#include <execinfo.h>

namespace mongo {
namespace {

class HeapProfiler {
private:
    std::vector<tcmalloc::MallocExtension::AllocationProfilingToken> profileTokens;
    std::atomic_size_t sampleIntervalBytes;
    std::atomic_size_t sampleBytesAllocated{0};

    struct StackInfo {
        int stackNum = 0;    // used for stack short name
        BSONObj stackObj{};  // symbolized representation
        int numFrames = 0;
        uint64_t activeBytes = 0;

        StackInfo(const tcmalloc::Profile::Sample& stackSample, int id) {
            stackNum = id;
            numFrames = stackSample.depth;

            // Generate a bson representation of our new stack.
            BSONArrayBuilder builder;
            std::string frameString;
            char buf[256];
            for (int i = 0; i < stackSample.depth; ++i) {
                if (!absl::Symbolize(stackSample.stack[i], buf, sizeof(buf))) {
                    // Fall back to frameString as stringified `void*`.
                    std::ostringstream s;
                    s << stackSample.stack[i];
                    frameString = s.str();
                } else {
                    frameString.assign(buf);
                }
                builder.append(frameString);
                frameString.clear();
            }

            stackObj = builder.obj();
            LOGV2(23158,
                  "heapProfile stack {stackNum}: {stackObj}",
                  "heapProfile stack",
                  "stackNum"_attr = stackNum,
                  "stackObj"_attr = stackObj);
        }
    };

    uint32_t StackHash(const tcmalloc::Profile::Sample& stackSample) {
        uint32_t hash;
        MurmurHash3_x86_32(stackSample.stack, stackSample.depth * sizeof(void*), 0, &hash);
        return hash;
    }

    //
    // Generate serverStatus section
    //
    bool logGeneralStats = true;  // first time only
    std::unordered_map<uint32_t, StackInfo*> stackInfoMap;

    // In order to reduce load on ftdc we track the stacks we deem important enough to emit
    // once a stack is deemed "important" it remains important from that point on.
    // "Important" is a sticky quality to improve the stability of the set of stacks we emit,
    // and we always emit them in stackNum order, greatly improving ftdc compression efficiency.
    std::set<StackInfo*, bool (*)(StackInfo*, StackInfo*)> importantStacks{
        [](StackInfo* a, StackInfo* b) -> bool { return a->stackNum < b->stackNum; }};
    int numImportantSamples = 0;                // samples currently included in importantStacks
    const int kMaxImportantSamples = 4 * 3600;  // reset every 4 hours at default 1 sample / sec

    void _generateServerStatusSection(BSONObjBuilder& builder) {
        // Compute and log some informational stats first time through
        if (logGeneralStats) {
            LOGV2(23159,
                  "Generating heap profiler serverStatus: sampleIntervalBytes "
                  "heapProfilingSampleIntervalBytes}; ",
                  "Generating heap profiler serverStatus",
                  "heapProfilingSampleIntervalBytes"_attr = HeapProfilingSampleIntervalBytes);
            LOGV2(23160, "Following stack trace is for heap profiler informational purposes");
            printStackTrace();
            logGeneralStats = false;
        }

        // Get a live snapshot profile of the current heap usage
        int64_t totalActiveBytes = 0;
        std::vector<StackInfo*> stackInfos;
        std::set<StackInfo*, bool (*)(StackInfo*, StackInfo*)> activeStacks{
            [](StackInfo* a, StackInfo* b) -> bool { return a->stackNum < b->stackNum; }};
        auto heapProfile = tcmalloc::MallocExtension::SnapshotCurrent(tcmalloc::ProfileType::kHeap);
        heapProfile.Iterate([&](const tcmalloc::Profile::Sample& sample) {
            totalActiveBytes += sample.sum;
            // Compute backtrace hash of sample stack
            uint32_t stackHash = StackHash(sample);
            StackInfo* stackInfo = stackInfoMap[stackHash];
            // If this is a new stack, store in our stack map
            if (stackInfo == nullptr) {
                stackInfo = new StackInfo(sample, stackInfoMap.size());
                stackInfoMap[stackHash] = stackInfo;
            }
            auto activeStackSearch = activeStacks.find(stackInfo);
            if (activeStackSearch != activeStacks.end()) {
                stackInfo->activeBytes += sample.sum;
            } else {
                activeStacks.insert(stackInfo);
                stackInfos.push_back(stackInfo);
                stackInfo->activeBytes = sample.sum;
            }
        });

        // Get the series of allocation samples to this point
        auto currentToken = std::move(profileTokens.back());
        profileTokens.pop_back();
        auto allocProfile = std::move(currentToken).Stop();
        // Start a new allocation profile session for the next invocation
        auto newToken = tcmalloc::MallocExtension::StartAllocationProfiling();
        profileTokens.push_back(std::move(newToken));

        // Sum all the allocations performed (of what we sampled)
        int64_t allocatedBytes = 0;
        allocProfile.Iterate(
            [&](const tcmalloc::Profile::Sample& sample) { allocatedBytes += sample.sum; });
        sampleBytesAllocated += allocatedBytes;

        BSONObjBuilder statsBuilder(builder.subobjStart("stats"));
        statsBuilder.appendNumber("totalActiveBytes", static_cast<long long>(totalActiveBytes));
        statsBuilder.appendNumber("bytesAllocated", static_cast<long long>(sampleBytesAllocated));
        statsBuilder.appendNumber("numStacks", static_cast<long long>(stackInfoMap.size()));
        statsBuilder.doneFast();

        // Sort the stacks and find enough stacks to account for at least 99% of the active bytes
        // deem any stack that has ever met this criterion as "important".
        auto sortByActiveBytes = [](StackInfo* a, StackInfo* b) -> bool {
            return a->activeBytes > b->activeBytes;
        };
        std::stable_sort(stackInfos.begin(), stackInfos.end(), sortByActiveBytes);
        size_t threshold = totalActiveBytes * 0.99;
        size_t cumulative = 0;
        for (auto it = stackInfos.begin(); it != stackInfos.end(); ++it) {
            StackInfo* stackInfo = *it;
            importantStacks.insert(stackInfo);
            cumulative += stackInfo->activeBytes;
            if (cumulative > threshold)
                break;
        }

        // Build the stacks subsection by emitting a sample of stacks that were live at a peak of
        // total heap usage.
        BSONObjBuilder stacksBuilder(builder.subobjStart("stacks"));
        for (auto it = importantStacks.begin(); it != importantStacks.end(); ++it) {
            StackInfo* stackInfo = *it;
            std::ostringstream shortName;
            shortName << "stack" << stackInfo->stackNum;
            BSONObjBuilder stackBuilder(stacksBuilder.subobjStart(shortName.str()));
            stackBuilder.appendNumber("activeBytes",
                                      static_cast<long long>(stackInfo->activeBytes));
        }
        stacksBuilder.doneFast();

        // importantStacks grows monotonically, so it can accumulate unneeded stacks,
        // so we clear it periodically.
        if (++numImportantSamples >= kMaxImportantSamples) {
            LOGV2(23161, "Clearing importantStacks");
            importantStacks.clear();
            numImportantSamples = 0;
        }
    }

public:
    static HeapProfiler* heapProfiler;

    HeapProfiler() {
        sampleIntervalBytes = HeapProfilingSampleIntervalBytes;
        tcmalloc::MallocExtension::SetProfileSamplingRate(sampleIntervalBytes);
        auto profileToken = tcmalloc::MallocExtension::StartAllocationProfiling();
        profileTokens.push_back(std::move(profileToken));
    }

    static void generateServerStatusSection(BSONObjBuilder& builder) {
        if (heapProfiler)
            heapProfiler->_generateServerStatusSection(builder);
    }
};

//
// serverStatus section
//

class HeapProfilerServerStatusSection final : public ServerStatusSection {
public:
    HeapProfilerServerStatusSection() : ServerStatusSection("heapProfile") {}

    bool includeByDefault() const override {
        return HeapProfilingEnabled;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        HeapProfiler::generateServerStatusSection(builder);
        return builder.obj();
    }

} heapProfilerServerStatusSection;


//
// startup
//
HeapProfiler* HeapProfiler::heapProfiler;

MONGO_INITIALIZER_GENERAL(StartHeapProfiling, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    if (HeapProfilingEnabled)
        HeapProfiler::heapProfiler = new HeapProfiler();
}

}  // namespace
}  // namespace mongo

#endif  // defined(_POSIX_VERSION)
