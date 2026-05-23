// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstddef>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_MSVC_LANG)
#define WHITEOUT_CPLUSPLUS _MSVC_LANG
#else
#define WHITEOUT_CPLUSPLUS __cplusplus
#endif

#if defined(__has_include)
#if WHITEOUT_CPLUSPLUS >= 201703L && __has_include(<optional>)
#define WHITEOUT_HAS_STD_OPTIONAL 1
#endif
#if WHITEOUT_CPLUSPLUS >= 202002L && __has_include(<span>)
#define WHITEOUT_HAS_STD_SPAN 1
#endif
#endif

#if defined(WHITEOUT_HAS_STD_OPTIONAL)
#include <optional>
#if !defined(__cpp_lib_optional)
#undef WHITEOUT_HAS_STD_OPTIONAL
#endif
#endif

#if defined(WHITEOUT_HAS_STD_OPTIONAL)
#else
namespace std {

struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};

static const nullopt_t nullopt = nullopt_t(0);

class bad_optional_access : public logic_error {
public:
    bad_optional_access() : logic_error("bad optional access") {}
};

template <typename T>
class optional {
public:
    typedef T value_type;

    optional() noexcept : has_value_(false) {}
    optional(nullopt_t) noexcept : has_value_(false) {}

    optional(const T& value) : has_value_(false) {
        emplace(value);
    }

    optional(T&& value) : has_value_(false) {
        emplace(std::move(value));
    }

    optional(const optional& other) : has_value_(false) {
        if (other.has_value_) {
            emplace(*other);
        }
    }

    optional(optional&& other) : has_value_(false) {
        if (other.has_value_) {
            emplace(std::move(*other));
        }
    }

    ~optional() {
        reset();
    }

    optional& operator=(nullopt_t) {
        reset();
        return *this;
    }

    optional& operator=(const optional& other) {
        if (this == &other) {
            return *this;
        }

        if (other.has_value_) {
            if (has_value_) {
                **this = *other;
            } else {
                emplace(*other);
            }
        } else {
            reset();
        }

        return *this;
    }

    optional& operator=(optional&& other) {
        if (this == &other) {
            return *this;
        }

        if (other.has_value_) {
            if (has_value_) {
                **this = std::move(*other);
            } else {
                emplace(std::move(*other));
            }
        } else {
            reset();
        }

        return *this;
    }

    optional& operator=(const T& value) {
        if (has_value_) {
            **this = value;
        } else {
            emplace(value);
        }
        return *this;
    }

    optional& operator=(T&& value) {
        if (has_value_) {
            **this = std::move(value);
        } else {
            emplace(std::move(value));
        }
        return *this;
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        reset();
        ::new (static_cast<void*>(&storage_)) T(std::forward<Args>(args)...);
        has_value_ = true;
        return **this;
    }

    void reset() noexcept {
        if (has_value_) {
            ptr()->~T();
            has_value_ = false;
        }
    }

    bool has_value() const noexcept {
        return has_value_;
    }

    explicit operator bool() const noexcept {
        return has_value_;
    }

    T& value() {
        if (!has_value_) {
            std::terminate();
        }
        return *ptr();
    }

    const T& value() const {
        if (!has_value_) {
            std::terminate();
        }
        return *ptr();
    }

    T& operator*() {
        return *ptr();
    }

    const T& operator*() const {
        return *ptr();
    }

    T* operator->() {
        return ptr();
    }

    const T* operator->() const {
        return ptr();
    }

private:
    T* ptr() {
        return reinterpret_cast<T*>(&storage_);
    }

    const T* ptr() const {
        return reinterpret_cast<const T*>(&storage_);
    }

    typename aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool has_value_;
};

} // namespace std
#endif

#if defined(WHITEOUT_HAS_STD_SPAN)
#include <span>
#if !defined(__cpp_lib_span)
#undef WHITEOUT_HAS_STD_SPAN
#endif
#endif

#if defined(WHITEOUT_HAS_STD_SPAN)
#else
namespace std {

template <typename T>
class span {
public:
    typedef T element_type;
    typedef typename remove_cv<T>::type value_type;
    typedef size_t size_type;
    typedef T* pointer;
    typedef T& reference;
    typedef pointer iterator;
    typedef pointer const_iterator;

    span() noexcept : data_(nullptr), size_(0) {}

    span(pointer ptr, size_type count) : data_(ptr), size_(count) {}

    span(pointer first, pointer last) : data_(first), size_(static_cast<size_type>(last - first)) {}

    template <size_t N>
    span(element_type (&arr)[N]) : data_(arr), size_(N) {}

    template <typename U, typename Alloc,
              typename enable_if<is_convertible<U*, T*>::value, int>::type = 0>
    span(vector<U, Alloc>& v) : data_(v.data()), size_(v.size()) {}

    template <typename U, typename Alloc,
              typename enable_if<is_convertible<const U*, T*>::value, int>::type = 0>
    span(const vector<U, Alloc>& v) : data_(v.data()), size_(v.size()) {}

    template <typename U, typename enable_if<is_convertible<U*, T*>::value, int>::type = 0>
    span(const span<U>& other) : data_(other.data()), size_(other.size()) {}

    iterator begin() const noexcept {
        return data_;
    }

    iterator end() const noexcept {
        return data_ + size_;
    }

    reference operator[](size_type index) const {
        return data_[index];
    }

    pointer data() const noexcept {
        return data_;
    }

    size_type size() const noexcept {
        return size_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

private:
    pointer data_;
    size_type size_;
};

} // namespace std
#endif

#undef WHITEOUT_HAS_STD_OPTIONAL
#undef WHITEOUT_HAS_STD_SPAN
#undef WHITEOUT_CPLUSPLUS