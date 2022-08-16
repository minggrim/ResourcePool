#ifndef _MK_RCPOOL_
#define _MK_RPPOOL_

#include <unordered_map>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <functional>
#include <cstddef>


namespace mklib {
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

    GetWrapper get() {
        GetWrapper gw { inner_pool_, inner_pool_->inner_get()};
        return gw;
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
        std::shared_ptr<INST_T> inner_get() {
            std::unique_lock<std::mutex> uq_cvlock(cvlock);
            get_item:
            if (unused.size() == 0 && cur_sz >= max_limit) {
                cv.wait(uq_cvlock);
                goto get_item;
            }

            std::shared_ptr<INST_T> this_get = nullptr;

            if (unused.size()) {
                this_get = unused.begin()->second;
                unused.erase(unused.begin());
            }
            else {
                this_get = factory();
                cur_sz++;
            }

            used.emplace(this_get.get(), this_get);

            return this_get;
        }

        void inner_put(std::shared_ptr<INST_T> ptr) {
            std::unique_lock<std::mutex> uq_cvlock(cvlock);
            auto p = used.find(ptr.get());
            if (p == used.end()) return;

            used.erase(p);

            // back to used if < idle_limit
            if (used.size() < idle_limit) {
                unused.emplace(p->second.get(), ptr);
            }
            else {
                cur_sz--;
            }

            uq_cvlock.unlock();
            cv.notify_one();
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
