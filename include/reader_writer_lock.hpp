#pragma once

#include <zephyr/kernel.h>

class ReaderWriterLock {
public:
    ReaderWriterLock()
    {
        k_mutex_init(&gate_);
        k_mutex_init(&reader_count_mutex_);
        k_sem_init(&resource_, 1, 1);
    }

    ReaderWriterLock(const ReaderWriterLock &) = delete;
    ReaderWriterLock &operator=(const ReaderWriterLock &) = delete;

    void lock_shared()
    {
        k_mutex_lock(&gate_, K_FOREVER);
        k_mutex_lock(&reader_count_mutex_, K_FOREVER);
        if (reader_count_ == 0) {
            k_sem_take(&resource_, K_FOREVER);
        }
        ++reader_count_;
        k_mutex_unlock(&reader_count_mutex_);
        k_mutex_unlock(&gate_);
    }

    void unlock_shared()
    {
        k_mutex_lock(&reader_count_mutex_, K_FOREVER);
        --reader_count_;
        if (reader_count_ == 0) {
            k_sem_give(&resource_);
        }
        k_mutex_unlock(&reader_count_mutex_);
    }

    void lock()
    {
        k_mutex_lock(&gate_, K_FOREVER);
        k_sem_take(&resource_, K_FOREVER);
    }

    void unlock()
    {
        k_sem_give(&resource_);
        k_mutex_unlock(&gate_);
    }

    class SharedGuard {
    public:
        explicit SharedGuard(ReaderWriterLock &lock) : lock_(lock) { lock_.lock_shared(); }
        ~SharedGuard() { lock_.unlock_shared(); }

        SharedGuard(const SharedGuard &) = delete;
        SharedGuard &operator=(const SharedGuard &) = delete;

    private:
        ReaderWriterLock &lock_;
    };

    class ExclusiveGuard {
    public:
        explicit ExclusiveGuard(ReaderWriterLock &lock) : lock_(lock) { lock_.lock(); }
        ~ExclusiveGuard() { lock_.unlock(); }

        ExclusiveGuard(const ExclusiveGuard &) = delete;
        ExclusiveGuard &operator=(const ExclusiveGuard &) = delete;

    private:
        ReaderWriterLock &lock_;
    };

private:
    k_mutex gate_;
    k_mutex reader_count_mutex_;
    k_sem resource_;
    unsigned int reader_count_ = 0;
};
