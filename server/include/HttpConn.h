#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include "TcpConn.h"
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include "LogMacro.h"

/** @brief 解析 HTTP/1.1 并路由车辆、列车、调度和静态文件请求。 */
class HttpConn : public TcpConn {
public:
    /** @brief 创建处于请求行解析状态的连接。 */
    HttpConn();
    /** @brief 释放 HTTP 连接资源。 */
    ~HttpConn();

    // ===== 核心接口 =====
    void process() override;
    /** @brief 判断本请求是否要求保持连接。 */
    bool isKeepAlive() const;

private:
    // ======================
    // HTTP解析
    // ======================
    enum class ParseState {
        REQUEST_LINE,
        HEADERS,
        BODY,
        ERROR,
        FINISH
    };

    /** @brief 增量解析读缓冲中的完整 HTTP 请求。 */
    void parse();
    /** @brief 解析方法、路径和协议版本，格式非法时返回 false。 */
    bool parseRequestLine(const std::string& line);
    /** @brief 解析并规范化单个请求头，格式非法时返回 false。 */
    bool parseHeader(const std::string& line);

    // ======================
    // 响应构建
    // ======================
    /** @brief 根据解析结果构建 API 或静态文件响应。 */
    void buildResponse();
    /** @brief 安全地映射并返回静态文件。 */
    void buildFileResponse();
    /** @brief 尝试匹配一个 API 路由。 */
    bool handleAPI();
    /** @brief 注册单辆车辆。 */
    bool handleVehicleRegister();
    /** @brief 更新单辆车辆。 */
    bool handleVehicleUpdate();
    /** @brief 删除单辆车辆。 */
    bool handleVehicleDelete();
    /** @brief 接收游戏端完整车辆快照。 */
    bool handleGameVehicleSnapshot();
    // 游戏 Mod 的铁路数据接口：线路低频全量上报，列车高频批量上报，
    // RELEASE 命令采用 query -> ACK -> cleared 三阶段生命周期。
    /** @brief 接收全量列车遥测并触发一次规划。 */
    bool handleTrainSnapshot();
    /** @brief 接收线路几何并重建拓扑。 */
    bool handleLineRouteSnapshot();
    /** @brief 查询待执行调度命令且不消费命令。 */
    bool handleDispatchCommandQuery();
    /** @brief 接收命令接受或拒绝确认。 */
    bool handleDispatchCommandAck();
    /** @brief 接收列车尾部离开受控资源事件。 */
    bool handleDispatchResourceCleared();
    /** @brief 处理全部 GET 查询。 */
    bool handleGet();
    /** @brief 创建带 JSON Content-Type 的内存响应。 */
    void buildJsonResponse(int code, const std::string& status, const std::string& body);
    /** @brief 尝试从 Redis 返回预序列化 JSON。 */
    bool tryBuildCachedJsonResponse(const char* key);
    /** @brief 以短 TTL 缓存 JSON 响应。 */
    void cacheJsonResponse(const char* key, const std::string& body);
    /** @brief 删除受写操作影响的响应缓存。 */
    void invalidateRedisCaches(std::initializer_list<const char*> keys);
    // ======================
    // 工具函数
    // ======================
    /** @brief 根据扩展名推断 MIME 类型。 */
    std::string getFileType(const std::string& path);
    /** @brief 规范化 URL 路径并阻止目录穿越。 */
    std::string safePath(const std::string& path);

private:
    // ===== 解析状态 =====
    ParseState state_;

    std::string method_;
    std::string path_;
    std::string version_;

    std::unordered_map<std::string, std::string> header_r_;
    std::string body_r_;

    bool keep_alive_;

    // ===== writev缓冲 =====
    std::string header_;   // HTTP响应头
    std::string body_;     // 小数据（404等）

    size_t content_length_ = 0;

    // ===== mmap文件 =====
    struct stat file_stat_;

};

#endif
