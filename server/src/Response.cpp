#include "Response.h"

#include <unistd.h>
#include <fcntl.h>
#include <sstream>

// ======================================
// MemoryResponse
// ======================================

MemoryResponse::MemoryResponse(const std::string& header, const std::string& body) {
    data_ = header + body;
}

bool MemoryResponse::nextIovec(struct iovec& out) {
    if (done())
        return false;

    out.iov_base = (void*)(data_.data() + offset_);
    out.iov_len  = data_.size() - offset_;

    return true;
}

void MemoryResponse::consume(size_t len) {
    offset_ = std::min(offset_ + len, data_.size());
}

bool MemoryResponse::done() const {
    return offset_ >= data_.size();
}

// ======================================
// FileResponse
// ======================================

FileResponse::FileResponse(const std::string& header, int fd, size_t size)
    : header_(header), file_data_(nullptr), file_size_(size) {
    if (size > 0) {
        void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            file_size_ = 0;
            file_data_ = nullptr;
            valid_ = false;
        } else {
            file_data_ = static_cast<char*>(p);
        }
    }
}

FileResponse::~FileResponse() {
    if (file_data_ && file_size_ > 0) {
        munmap(file_data_, file_size_);
    }
} 

bool FileResponse::nextIovec(struct iovec& out) {
    // header 未发送完
    if (header_offset_ < header_.size()) {
        out.iov_base = (void*)(header_.data() + header_offset_);
        out.iov_len = header_.size() - header_offset_;

        return true;
    }

    // file 未发送完
    if (file_offset_ < file_size_) {
        out.iov_base = file_data_ + file_offset_;
        out.iov_len = file_size_ - file_offset_;

        return true;
    }

    return false;
}

void FileResponse::consume(size_t len) {
    // 先消费 header
    if (header_offset_ < header_.size()) {
        size_t remain = header_.size() - header_offset_;

        if (len < remain) {
            header_offset_ = std::min(header_offset_ + len, header_.size());
            return;
        }

        header_offset_ = header_.size();
        len -= remain;
    }

    // 再消费 file
    file_offset_ = std::min(file_offset_ + len, file_size_);
}

bool FileResponse::done() const {
    return
        header_offset_ >= header_.size() && file_offset_ >= file_size_;
}

bool FileResponse::valid() const {
    return valid_;
}

// ======================================
// ChunkedResponse
// ======================================

ChunkedResponse::ChunkedResponse() {}

void ChunkedResponse::pushChunk(const std::string& chunk) {
    std::stringstream ss;

    ss << std::hex << chunk.size()
       << "\r\n"
       << chunk
       << "\r\n";

    chunks_.push(ss.str());
}

bool ChunkedResponse::nextIovec(struct iovec& out) {
    if (chunks_.empty())
        return false;

    auto& front = chunks_.front();

    out.iov_base = (void*)(front.data() + offset_);
    out.iov_len = front.size() - offset_;

    return true;
}

void ChunkedResponse::consume(size_t len) {
    if (chunks_.empty())
        return;

    offset_ += len;

    auto& front = chunks_.front();

    if (offset_ >= front.size()) {
        chunks_.pop();
        offset_ = 0;
    }
}

bool ChunkedResponse::done() const {
    return finished_ && chunks_.empty();
}
