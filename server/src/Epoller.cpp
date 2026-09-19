#include "Epoller.h"
#include <unistd.h>

Epoller::Epoller(int maxEvent) {
    epfd_ = epoll_create(1);
    events_.resize(maxEvent);
}

Epoller::~Epoller() {
    close(epfd_);
}

bool Epoller::addFd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Epoller::modFd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
} 

bool Epoller::delFd(int fd) {
    return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
}

int Epoller::wait(int timeout) {
    return epoll_wait(epfd_, events_.data(), events_.size(), timeout);
}

epoll_event Epoller::getEvent(int i) {
    return events_[i];
}