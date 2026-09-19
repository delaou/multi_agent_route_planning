#pragma once

#include "VehicleState.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class DBConnectionPool;

/** @brief 管理车辆最新快照，并可整体保存到 MySQL。 */
class VehicleManager {
public:
    /** @brief 创建空的车辆容器。 */
    VehicleManager() = default;
    /** @brief 释放车辆容器；数据库连接归连接池所有。 */
    ~VehicleManager() = default;
    /** @brief 返回进程内共享的车辆管理器。 */
    static VehicleManager& instance();
    /** @brief 绑定数据库并加载已保存车辆。 */
    bool enablePersistence(DBConnectionPool* pool);
    /** @brief 在事务中保存当前完整车辆快照。 */
    bool saveAll();
    /** @brief 新增不存在的车辆。 */
    bool registerVehicle(const VehicleState& state);
    /** @brief 更新已经存在的车辆。 */
    bool updateVehicle(const VehicleState& state);
    /** @brief 删除指定车辆。 */
    bool removeVehicle(const std::string& id);
    /** @brief 用游戏端上传的全量快照原子替换内存数据。 */
    void replaceSnapshot(VehicleSnapshot snapshot);
    /** @brief 按 ID 查询车辆副本。 */
    std::optional<VehicleState> getVehicle(const std::string& id) const;
    /** @brief 返回按内部容器收集的全部车辆副本。 */
    std::vector<VehicleState> getAllVehicles() const;
    /** @brief 返回车辆、地图边界和时间戳的完整快照。 */
    VehicleSnapshot getSnapshot() const;
    /** @brief 判断车辆是否存在。 */
    bool hasVehicle(const std::string& id) const;
    /** @brief 返回车辆数量。 */
    size_t size() const;

private:
    std::unordered_map<std::string, VehicleState> vehicles_;
    uint64_t snapshot_timestamp_ = 0;
    GameMapBounds map_bounds_;
    mutable std::mutex mutex_;
    DBConnectionPool* db_pool_ = nullptr;
};
