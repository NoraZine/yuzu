// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_device.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/wrapper.h"

namespace Vulkan {

using VideoCore::QueryType;

namespace {

constexpr std::array QUERY_TARGETS = {VK_QUERY_TYPE_OCCLUSION};

constexpr VkQueryType GetTarget(QueryType type) {
    return QUERY_TARGETS[static_cast<std::size_t>(type)];
}

} // Anonymous namespace

QueryPool::QueryPool(const VKDevice& device_, VKScheduler& scheduler, QueryType type_)
    : ResourcePool{scheduler.GetMasterSemaphore(), GROW_STEP}, device{device_}, type{type_} {}

QueryPool::~QueryPool() = default;

std::pair<VkQueryPool, u32> QueryPool::Commit() {
    std::size_t index;
    do {
        index = CommitResource();
    } while (usage[index]);
    usage[index] = true;

    return {*pools[index / GROW_STEP], static_cast<u32>(index % GROW_STEP)};
}

void QueryPool::Allocate(std::size_t begin, std::size_t end) {
    usage.resize(end);

    pools.push_back(device.GetLogical().CreateQueryPool({
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queryType = GetTarget(type),
        .queryCount = static_cast<u32>(end - begin),
        .pipelineStatistics = 0,
    }));
}

void QueryPool::Reserve(std::pair<VkQueryPool, u32> query) {
    const auto it =
        std::find_if(pools.begin(), pools.end(), [query_pool = query.first](vk::QueryPool& pool) {
            return query_pool == *pool;
        });
    ASSERT(it != std::end(pools));

    const std::ptrdiff_t pool_index = std::distance(std::begin(pools), it);
    usage[pool_index * GROW_STEP + static_cast<std::ptrdiff_t>(query.second)] = false;
}

VKQueryCache::VKQueryCache(VideoCore::RasterizerInterface& rasterizer,
                           Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& gpu_memory,
                           const VKDevice& device, VKScheduler& scheduler)
    : VideoCommon::QueryCacheBase<VKQueryCache, CachedQuery, CounterStream,
                                  HostCounter>{rasterizer, maxwell3d, gpu_memory},
      device{device}, scheduler{scheduler}, query_pools{
                                                QueryPool{device, scheduler,
                                                          QueryType::SamplesPassed},
                                            } {}

VKQueryCache::~VKQueryCache() {
    // TODO(Rodrigo): This is a hack to destroy all HostCounter instances before the base class
    // destructor is called. The query cache should be redesigned to have a proper ownership model
    // instead of using shared pointers.
    for (size_t query_type = 0; query_type < VideoCore::NumQueryTypes; ++query_type) {
        auto& stream = Stream(static_cast<QueryType>(query_type));
        stream.Update(false);
        stream.Reset();
    }
}

std::pair<VkQueryPool, u32> VKQueryCache::AllocateQuery(QueryType type) {
    return query_pools[static_cast<std::size_t>(type)].Commit();
}

void VKQueryCache::Reserve(QueryType type, std::pair<VkQueryPool, u32> query) {
    query_pools[static_cast<std::size_t>(type)].Reserve(query);
}

HostCounter::HostCounter(VKQueryCache& cache, std::shared_ptr<HostCounter> dependency,
                         QueryType type)
    : VideoCommon::HostCounterBase<VKQueryCache, HostCounter>{std::move(dependency)}, cache{cache},
      type{type}, query{cache.AllocateQuery(type)}, tick{cache.Scheduler().CurrentTick()} {
    const vk::Device* logical = &cache.Device().GetLogical();
    cache.Scheduler().Record([logical, query = query](vk::CommandBuffer cmdbuf) {
        logical->ResetQueryPoolEXT(query.first, query.second, 1);
        cmdbuf.BeginQuery(query.first, query.second, VK_QUERY_CONTROL_PRECISE_BIT);
    });
}

HostCounter::~HostCounter() {
    cache.Reserve(type, query);
}

void HostCounter::EndQuery() {
    cache.Scheduler().Record(
        [query = query](vk::CommandBuffer cmdbuf) { cmdbuf.EndQuery(query.first, query.second); });
}

u64 HostCounter::BlockingQuery() const {
    if (tick >= cache.Scheduler().CurrentTick()) {
        cache.Scheduler().Flush();
    }
    u64 data;
    const VkResult result = cache.Device().GetLogical().GetQueryResults(
        query.first, query.second, 1, sizeof(data), &data, sizeof(data),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    switch (result) {
    case VK_SUCCESS:
        return data;
    case VK_ERROR_DEVICE_LOST:
        cache.Device().ReportLoss();
        [[fallthrough]];
    default:
        throw vk::Exception(result);
    }
}

} // namespace Vulkan
