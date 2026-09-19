#ifndef RESPONSE_H
#define RESPONSE_H

#include <string>
#include <queue>
#include <sys/uio.h>
#include <sys/mman.h>

/** @brief 可分段发送的 HTTP 响应抽象。 */
class HttpResponse {
public:
    /** @brief 返回下一段尚未发送的数据视图。 */
    virtual bool nextIovec(struct iovec& out) = 0;
    /** @brief 在 writev 成功后推进已发送字节数。 */
    virtual void consume(size_t len) = 0;
    /** @brief 判断响应是否全部发送完成。 */
    virtual bool done() const = 0;
    /** @brief 允许通过基类安全销毁具体响应。 */
    virtual ~HttpResponse() {}
};

// =========================
// 内存响应
// =========================

/** @brief 将响应头和小响应体合并保存在内存中的响应。 */
class MemoryResponse : public HttpResponse {
public:
    /** @brief 复制响应头和响应体。 */
    MemoryResponse(const std::string& header, const std::string& body);
    /** @brief 返回尚未发送的内存范围。 */
    bool nextIovec(struct iovec& out) override;
    /** @brief 推进内存发送偏移。 */
    void consume(size_t len) override;
    /** @brief 判断全部内存数据是否发送完成。 */
    bool done() const override;

private:
    std::string data_;
    size_t offset_ = 0;
};

// =========================
// 文件响应
// =========================

/** @brief 使用 mmap 零拷贝读取静态文件的响应。 */
class FileResponse : public HttpResponse {
public:
    /** @brief 保存响应头并映射给定文件。 */
    FileResponse(const std::string& header, int fd, size_t size);
    /** @brief 解除文件映射。 */
    ~FileResponse();
    /** @brief 依次返回响应头或文件的未发送范围。 */
    bool nextIovec(struct iovec& out) override;
    /** @brief 推进当前响应段的偏移。 */
    void consume(size_t len) override;
    /** @brief 判断响应头和文件是否都发送完成。 */
    bool done() const override;
    /** @brief 判断文件映射是否成功。 */
    bool valid() const;

private:
    std::string header_;

    size_t header_offset_ = 0;

    char* file_data_ = nullptr;
    size_t file_size_ = 0;
    size_t file_offset_ = 0;
    bool valid_ = true;
};

// =========================
// Chunked 响应
// =========================

/** @brief 保存多个内存块并顺序输出的响应。 */
class ChunkedResponse : public HttpResponse {
public:
    /** @brief 创建空分块响应。 */
    ChunkedResponse();
    /** @brief 向队尾添加一个待发送块。 */
    void pushChunk(const std::string& chunk);
    /** @brief 返回队首块尚未发送的范围。 */
    bool nextIovec(struct iovec& out) override;
    /** @brief 消费字节，并在块完成时弹出队首。 */
    void consume(size_t len) override;
    /** @brief 判断是否已经标记结束且没有剩余块。 */
    bool done() const override;

private:
    std::queue<std::string> chunks_;
    size_t offset_ = 0;
    bool finished_ = false;
};

#endif
