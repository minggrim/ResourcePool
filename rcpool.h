#ifndef _MK_RCPOOL_
#define _MK_RPPOOL_

#include <unordered_map>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <functional>
#include <cstddef>
#include <exception>
#include <string>
#include <chrono>


namespace mklib {

namespace {
    class GenericResourceException : public std::exception {
        public:
            GenericResourceException(const std::string & m) : msg_(m) {}
            const char * what() const noexcept {return msg_.c_str();}
        private:
            std::string msg_;
    };
}

template <class INST_T>
class RCPool
{
public:
    class GetWrapper {
        public:
            GetWrapper(
                std::shared_ptr<typename RCPool<INST_T>::InnerRCPool> pool, std::shared_ptr<INST_T> ptr) :
                rcpool_(pool), ptr_(ptr)
            {}

            // disallow copy
            GetWrapper(const GetWrapper & rhs) = delete;
            GetWrapper & operator=(const GetWrapper & rhs) = delete;

            // allow move
            GetWrapper(GetWrapper && rhs)
                : rcpool_(rhs.rcpool_), ptr_(rhs.ptr_)
            {
                // this obj is responsible for release.
                rhs.ptr_ = nullptr;
            }

            GetWrapper & operator=(GetWrapper && rhs) {
                // release mine
                if (ptr_) {
                    rcpool_.inner_put(ptr_);
                    ptr_ = nullptr;
                }

                // inherit other
                ptr_ = rhs.ptr_;

                // rhs doesn't need release
                rhs.ptr_ = nullptr;

                return *this;
            }

            ~GetWrapper() {
                // need release to pool
                if (ptr_) {
                    rcpool_->inner_put(ptr_);
                }
            }

            INST_T * operator->() {
                return ptr_.get();
            }

            INST_T * get() {
                return ptr_.get();
            }

            operator bool() {
                return ptr_;
            }

        private:
            std::shared_ptr<typename RCPool<INST_T>::InnerRCPool> rcpool_;
            std::shared_ptr<INST_T> ptr_;
    };

    template <class... Args>
    RCPool(size_t idle_limit_, size_t max_limit_, Args&&... _args) {
        inner_pool_ = std::make_shared<InnerRCPool> (idle_limit_, max_limit_, std::forward<Args>(_args)...);
    }

    // disallow copy
    RCPool(const RCPool & rhs) = delete;
    RCPool & operator=(const RCPool & rhs) = delete;

    // disallow copy
    RCPool(RCPool && rhs) = delete;
    RCPool & operator=(RCPool && rhs) = delete;

    virtual ~RCPool() {}

    GetWrapper get(uint32_t timeout_s = 0) {
        try {
            return { inner_pool_, inner_pool_->inner_get(timeout_s)};
        }
        catch (const GenericResourceException &e) {
            return { nullptr, nullptr };
        }
    }

    size_t size() {
        return  inner_pool_->cur_sz;
    }

private:
    class InnerRCPool {
        public:
        template <class... Args>
        InnerRCPool(size_t idle_limit_, size_t max_limit_, Args&&... _args) {
            cur_sz = 0;
            idle_limit = idle_limit_;
            max_limit = std::max(idle_limit_, max_limit_);
            factory = [_args...]() -> std::shared_ptr<INST_T> {
                return std::make_shared<INST_T>(_args...);
            };
        }

        // disallow copy
        InnerRCPool(const InnerRCPool & rhs) = delete;
        InnerRCPool & operator=(const InnerRCPool & rhs) = delete;

        // disallow copy
        InnerRCPool(InnerRCPool && rhs) = delete;
        InnerRCPool & operator=(InnerRCPool && rhs) = delete;

        private:
        friend class RCPool;
        friend class GetWrapper;

        std::shared_ptr<INST_T> inner_get(uint32_t timeout_s) {

            std::unique_lock<std::mutex> uq_cvlock(cvlock);
            if (timeout_s) {
                auto t = cv.wait_until(
                            uq_cvlock,
                            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s),
                            [this]() -> bool { return resource_available(); }
                         );
                if (!t) {
                    throw GenericResourceException("Timedout");
                }
            }
            else {
                cv.wait(
                    uq_cvlock,
                    [this]() -> bool { return resource_available(); }
                );
            }

            std::shared_ptr<INST_T> this_get = nullptr;

            if (unused.size()) {
                this_get = unused.begin()->second;
                unused.erase(unused.begin());
            }
            else {
                try {
                    this_get = factory();
                }
                catch (const std::exception & e) {
                    // wrap throw
                    throw GenericResourceException(e.what());
                }
                cur_sz++;
            }

            used.emplace(this_get.get(), this_get);

            return this_get;
        }

        void inner_put(std::shared_ptr<INST_T> ptr) {
            std::unique_lock<std::mutex> uq_cvlock(cvlock);
            auto p = used.find(ptr.get());
            if (p == used.end()) return;

            // back to used if < idle_limit
            if (used.size() > idle_limit) {
                cur_sz--;
            }
            else {
                unused.emplace(p->second.get(), ptr);
            }

            // erase from used
            used.erase(p);

            uq_cvlock.unlock();
            cv.notify_one();
        }

        bool resource_available() {
            return cur_sz < max_limit;
        }

        size_t idle_limit;
        size_t max_limit;
        size_t cur_sz;
        std::mutex cvlock;
        std::condition_variable cv;
        std::unordered_map<INST_T *, std::shared_ptr<INST_T>> used;
        std::unordered_map<INST_T *, std::shared_ptr<INST_T>> unused;
        std::function<std::shared_ptr<INST_T>()> factory;
    };

    std::shared_ptr<InnerRCPool> inner_pool_;
};
}
#endif
