#include "MetricsMgr.h"
#include <iostream>
#include <algorithm>
#include <sstream>

// ============================================================
// 计数器 — 递增，首次自动创建
// ============================================================

// 递增计数器。如果 name 尚未注册，自动创建后再递增
void MetricsMgr::IncrementCounter(const std::string& name,
    const std::string& help,
    int64_t val)
{
    // 先查是否已注册，已注册直接递增
    {
        std::lock_guard<std::mutex> lock(map_lock_);

        auto it = counters_.find(name);
        if (it != counters_.end())
        {
            it->second.value.fetch_add(val, std::memory_order_relaxed);
            return;
        }
    }

    // 未注册 → 创建后再递增
    GetOrCreateCounter(name, help).value.fetch_add(val, std::memory_order_relaxed);
}

// 读取计数器当前值。未注册返回 0
int64_t MetricsMgr::GetCounterValue(const std::string& name)
{
    std::lock_guard<std::mutex> lock(map_lock_);

    auto it = counters_.find(name);
    return (it != counters_.end())
        ? it->second.value.load(std::memory_order_relaxed)
        : 0;
}

// ============================================================
// 直方图 — 观察耗时，首次自动创建
// ============================================================

// 观察一次耗时。如果 name 尚未注册，自动创建后再写入
void MetricsMgr::ObserveHistogram(const std::string& name,
    int64_t elapsed_ms,
    const std::string& help,
    const std::vector<double>& buckets)
{
    // 匿名 lambda：将 elapsed_ms 写入直方图 h（找桶 + 累加）
    auto do_observe = [&](HistogramItem& h)
        {
            // 找到 elapsed_ms 落在哪个桶里
            bool found = false;
            for (size_t i = 0; i < h.bucket_upper.size(); ++i)
            {
                if (elapsed_ms <= h.bucket_upper[i])
                {
                    h.bucket_counts[i]++;     // 对应桶 +1
                    found = true;
                    break;
                }
            }
            // 所有桶都比 elapsed_ms 小 → 丢进最后一个桶
            if (!found && !h.bucket_counts.empty())
            {
                h.bucket_counts.back()++;
            }

            // 累加总耗时和总次数
            h.sum_ms += elapsed_ms;
            h.count++;
        };

    // ---- 第一步：检查这个直方图是否已注册 ----
    {
        std::lock_guard<std::mutex> lock(map_lock_);

        auto it = histograms_.find(name);
        if (it != histograms_.end())
        {
            // 已注册 → 直接写数据
            std::lock_guard<std::mutex> hlock(hist_lock_);
            do_observe(it->second);
            return;
        }
    }

    // ---- 第二步：未注册 → 先创建，再写数据 ----
    GetOrCreateHistogram(name, help, buckets);

    std::lock_guard<std::mutex> lock(map_lock_);
    auto it = histograms_.find(name);
    if (it != histograms_.end())
    {
        std::lock_guard<std::mutex> hlock(hist_lock_);
        do_observe(it->second);
    }
}

// ============================================================
// 获取 / 创建
// ============================================================

// 获取或创建计数器。已存在直接返回，不存在用 help 创建默认值 0
CounterItem& MetricsMgr::GetOrCreateCounter(const std::string& name,
    const std::string& help)
{
    std::lock_guard<std::mutex> lock(map_lock_);

    auto [it, inserted] = counters_.try_emplace(name);
    if (inserted)
    {
        it->second.name = name;
        it->second.help = help;
        it->second.value.store(0, std::memory_order_relaxed);
    }
    return it->second;
}

// 获取或创建直方图。已存在直接返回，不存在用指定桶结构创建
HistogramItem& MetricsMgr::GetOrCreateHistogram(const std::string& name,
    const std::string& help,
    const std::vector<double>& buckets)
{
    std::lock_guard<std::mutex> lock(map_lock_);

    auto [it, inserted] = histograms_.try_emplace(name);
    if (inserted)
    {
        it->second.name = name;
        it->second.help = help;
        it->second.bucket_upper = buckets;
        it->second.bucket_counts.assign(buckets.size(), 0);
    }
    return it->second;
}

// ============================================================
// 默认桶：10ms / 50ms / 200ms / 500ms / 1000ms
// ============================================================

std::vector<double> MetricsMgr::DefaultBuckets()
{
    return { 10.0, 50.0, 200.0, 500.0, 1000.0 };
}

// ============================================================
// 序列化为 JSON
// ============================================================

// 输出所有计数器和直方图的完整 JSON，供 /api/metrics 路由返回
std::string MetricsMgr::CollectToJson()
{
    std::lock_guard<std::mutex> lock(map_lock_);

    std::ostringstream oss;
    oss << "{" << std::endl;

    // ---- counters ----
    oss << "  \"counters\": [" << std::endl;
    bool first = true;
    for (const auto& [name, item] : counters_)
    {
        if (!first) oss << "," << std::endl;
        first = false;

        oss << "    {"
            << "\"name\":\"" << name << "\","
            << "\"help\":\"" << item.help << "\","
            << "\"value\":" << item.value.load(std::memory_order_relaxed)
            << "}";
    }
    oss << std::endl << "  ]," << std::endl;

    // ---- histograms ----
    oss << "  \"histograms\": [" << std::endl;
    bool first_hist = true;
    {
        std::lock_guard<std::mutex> hlock(hist_lock_);
        for (const auto& [name, h] : histograms_)
        {
            if (!first_hist) oss << "," << std::endl;
            first_hist = false;

            oss << "    {" << std::endl;
            oss << "      \"name\":\"" << name << "\"," << std::endl;
            oss << "      \"help\":\"" << h.help << "\"," << std::endl;
            oss << "      \"buckets\": [" << std::endl;

            // 输出每个桶的命中次数
            int64_t sum_buckets = 0;
            for (size_t i = 0; i < h.bucket_upper.size(); ++i)
            {
                sum_buckets += h.bucket_counts[i];
                if (i > 0) oss << "," << std::endl;
                oss << "        {\"le\":" << h.bucket_upper[i]
                    << ",\"count\":" << h.bucket_counts[i] << "}";
            }

            // 超过最大桶的请求 → 归入 +Inf 桶
            int64_t overflow = h.count - sum_buckets;
            if (overflow < 0) overflow = 0;
            oss << "," << std::endl;
            oss << "        {\"le\":\"+Inf\",\"count\":" << overflow << "}" << std::endl;

            oss << "      ]," << std::endl;
            // 平均耗时（整数毫秒）
            oss << "      \"avg_ms\":"
                << (h.count > 0 ? (h.sum_ms / h.count) : 0) << ","
                << std::endl;
            oss << "      \"total\":" << h.count << std::endl;
            oss << "    }";
        }
    }
    oss << std::endl << "  ]" << std::endl;
    oss << "}" << std::endl;

    return oss.str();
}