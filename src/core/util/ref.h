// Intrusive reference counting for GL objects. GL objects outlive their names
// while still bound or attached somewhere, so every binding holds a RefPtr.
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace wmgl {

class RefCounted {
public:
    RefCounted() = default;
    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;
    void retain() const { refs_.fetch_add(1, std::memory_order_relaxed); }
    void release() const {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }

protected:
    virtual ~RefCounted() = default;

private:
    mutable std::atomic<uint32_t> refs_{0};
};

template <class T>
class RefPtr {
public:
    RefPtr() = default;
    RefPtr(std::nullptr_t) {}
    RefPtr(T* object) : ptr_(object) { if (ptr_) ptr_->retain(); }
    RefPtr(const RefPtr& other) : ptr_(other.ptr_) { if (ptr_) ptr_->retain(); }
    RefPtr(RefPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    template <class U>
    RefPtr(const RefPtr<U>& other) : ptr_(other.get()) { if (ptr_) ptr_->retain(); }
    ~RefPtr() { if (ptr_) ptr_->release(); }
    RefPtr& operator=(RefPtr other) noexcept { std::swap(ptr_, other.ptr_); return *this; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    bool operator==(const RefPtr& other) const { return ptr_ == other.ptr_; }
    bool operator!=(const RefPtr& other) const { return ptr_ != other.ptr_; }
    void reset() { RefPtr().swap(*this); }
    void swap(RefPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

private:
    T* ptr_ = nullptr;
};

template <class T, class... Args>
RefPtr<T> makeRef(Args&&... args) { return RefPtr<T>(new T(std::forward<Args>(args)...)); }

}  // namespace wmgl
