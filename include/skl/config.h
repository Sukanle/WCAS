#pragma once
#ifdef SKL_MACRO_UNUSED
#  define SKL_UNUSED_VAR(var) ((void)var)
#  define SKL_UNUSED_RET(fn) ((void)fn)
#endif

#ifndef SKL_CURRENT_VERSION
#  define SKL_CURRENT_VERSION "0.1.0"
#endif

#ifndef SKL_DELETE_COPYABLE
#  define SKL_DELETE_COPYABLE(CLASS)          \
      CLASS(const CLASS &) noexcept = delete; \
      CLASS &operator=(const CLASS &) noexcept = delete;
#endif

#ifndef SKL_DELETE_MOVEABLE
#  define SKL_DELETE_MOVEABLE(CLASS)     \
      CLASS(CLASS &&) noexcept = delete; \
      CLASS &operator=(CLASS &&) noexcept = delete;
#endif

#ifndef SKL_DEFAULT_COPYABLE
#  define SKL_DEFAULT_COPYABLE(CLASS)          \
      CLASS(const CLASS &) noexcept = default; \
      CLASS &operator=(const CLASS &) noexcept = default;
#endif

#ifndef SKL_DEFAULT_MOVEABLE
#  define SKL_DEFAULT_MOVEABLE(CLASS)     \
      CLASS(CLASS &&) noexcept = default; \
      CLASS &operator=(CLASS &&) noexcept = default;
#endif

#ifndef SKL_DEFAULT_COPYMOVE
#  define SKL_DEFAULT_COPYMOVE(CLASS) \
      SKL_DEFAULT_COPYABLE(CLASS)     \
      SKL_DEFAULT_MOVEABLE(CLASS)
#endif

#ifndef SKL_DELETE_COPYMOVE
#  define SKL_DELETE_COPYMOVE(CLASS) \
      SKL_DELETE_COPYABLE(CLASS)     \
      SKL_DELETE_MOVEABLE(CLASS)
#endif

#include <version>

#if defined(DEBUG) || defined(_DEBUG) || defined(__DEBUG__)
#  define SKL_DEBUG
#  import <fmt/format.h>
#  define SKL_PRINT(...) fmt::print(__VA_ARGS__)
#  define SKL_FORMAT(...) fmt::format(__VA_ARGS__)
#else
#  define SKL_PRINT(...)
#  define SKL_FORMAT(...)
#endif

#define SKL_INSTANCE(CLASS)                            \
    static auto instance() noexcept -> const CLASS & { \
        static error_category inst;                    \
        return inst;                                   \
    }
#define SKL_INSTANCE_H(CLASS) static auto instance() noexcept -> const CLASS &;
#define SKL_INSTANCE_CPP(CLASS)                               \
    static auto CLASS::instance() noexcept -> const CLASS & { \
        static error_category inst;                           \
        return inst;                                          \
    }
