#pragma once

#include "LineRouteAnalyzer.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// 某条线路穿过共线段的索引信息。low/high_segment 是该线路自身的索引范围，
// 实际入口和共线方向还要结合列车当前沿线路的运行方向计算。
struct CorridorTraversal {
    std::string corridor_id;
    std::string line_id;
    size_t low_segment = 0;
    size_t high_segment = 0;
    double low_position = 0.0;
    double high_position = 0.0;
    std::string occurrence_id;
    RouteDirectionRelation direction_relation = RouteDirectionRelation::SAME;
};

struct IntersectionTraversal {
    std::string intersection_id;
    std::string movement_id;
    std::string line_id;
    size_t segment_index = 0;
};

struct LineTopologySnapshot {
    // 每次完整线路快照成功替换后递增。游戏端可用它拒绝旧拓扑命令。
    uint64_t revision = 0;
    uint64_t algorithm_version = LINE_TOPOLOGY_ALGORITHM_VERSION;
    std::string algorithm_fingerprint;
    std::vector<LineRoute> routes;
    LineTopologyAnalysis analysis;
};

// 供高频状态查询使用，避免为几个计数复制数万线路点和完整分析结果。
struct LineTopologySummary {
    uint64_t revision = 0;
    uint64_t algorithm_version = LINE_TOPOLOGY_ALGORITHM_VERSION;
    std::string algorithm_fingerprint;
    size_t route_count = 0;
    size_t corridor_count = 0;
    size_t intersection_count = 0;
    size_t movement_count = 0;
};

// 保存线路几何和分析结果，并建立 line_id -> 受控资源的反向索引。
// 收到线路全量快照后一次性构建临时结果，成功后再交换，避免读线程看到半套拓扑。
class LineTopologyRegistry {
public:
    // 使用给定几何容差创建内部 LineRouteAnalyzer，不立即执行分析。
    explicit LineTopologyRegistry(LineRouteAnalyzerConfig config = {});

    // 校验并分析一份完整线路快照；全部成功后才原子替换旧拓扑。
    // 返回 false 时旧拓扑保持不变，reason 可接收可读错误原因。
    bool replaceRoutes(const std::vector<LineRoute>& routes,
                       std::string* reason = nullptr);
    // 启动恢复专用：在成功分析线路后把版本号恢复为 MySQL 中的历史版本。
    bool restoreRoutes(const std::vector<LineRoute>& routes,
                       uint64_t revision,
                       std::string* reason = nullptr);
    // 按 line_id 查询完整有序线路；结果为副本，调用方无需持有内部锁。
    std::optional<LineRoute> route(const std::string& line_id) const;
    // 查询该线路穿过的全部共线段及其在线路中的 segment 范围。
    std::vector<CorridorTraversal> corridorsForLine(
        const std::string& line_id) const;
    // 查询该线路穿过的全部交叉点 movement 及对应 segment 索引。
    std::vector<IntersectionTraversal> intersectionsForLine(
        const std::string& line_id) const;
    // 返回线路和完整分析结果的线程安全副本；线路按 line_id 排序。
    LineTopologySnapshot snapshot() const;
    // 返回版本和资源计数，不复制线路几何。
    LineTopologySummary summary() const;

private:
    LineRouteAnalyzer analyzer_;
    std::unordered_map<std::string, LineRoute> routes_;
    LineTopologyAnalysis analysis_;
    std::unordered_map<std::string, std::vector<CorridorTraversal>>
        corridors_by_line_;
    std::unordered_map<std::string, std::vector<IntersectionTraversal>>
        intersections_by_line_;
    uint64_t revision_ = 0;
    std::string algorithm_fingerprint_;
    mutable std::mutex mutex_;
};
