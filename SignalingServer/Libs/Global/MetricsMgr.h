#ifndef METRICSMGR_H
#define METRICSMGR_H

#include <atomic>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <sstream>

#include "Global/singletion.h"

struct CounterItem
{
    std::string name;
    std::string help;
    std::atomic<int64_t> value{ 0 };
};

struct HistogramItem
{
    std::string name;
    std::string help;
    std::vector<double> bucket_upper;                                                   // 桶上界，单位毫秒
    std::vector<int64_t> bucket_counts;                                                 // 每个桶命中次数，受 hist_lock_ 保护
    int64_t sum_ms{ 0 };                                                                // 总耗时（毫秒）
    int64_t count{ 0 };                                                                 // 总观察次数
};

class MetricsMgr : public Singleton<MetricsMgr>
{
public:
    // ---- 计数器 ----
    void IncrementCounter(const std::string& name,
        const std::string& help = "",
        int64_t val = 1);
    int64_t GetCounterValue(const std::string& name);

    // ---- 直方图 ----
    void ObserveHistogram(const std::string& name,
        int64_t elapsed_ms,
        const std::string& help = "",
        const std::vector<double>& buckets = DefaultBuckets());

    // ---- 工具 ----
    static std::vector<double> DefaultBuckets();

    // ---- 序列化 ----
    std::string CollectToJson();

private:
    CounterItem& GetOrCreateCounter(const std::string& name, const std::string& help);
    HistogramItem& GetOrCreateHistogram(const std::string& name, const std::string& help,
        const std::vector<double>& buckets);

    std::map<std::string, CounterItem> counters_;
    std::map<std::string, HistogramItem> histograms_;

    mutable std::mutex map_lock_;                                                       // 保护 counters_ / histograms_
    mutable std::mutex hist_lock_;                                                      // 保护 HistogramItem 内部计数
};

#endif // METRICSMGR_H