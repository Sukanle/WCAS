#pragma once
#if defined(_WIN32)
#  if defined(WCAS_BUILDING_LIBRARY)
#    define SKL_WCAS_API __declspec(dllexport)
#  else
#    define SKL_WCAS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define SKL_WCAS_API __attribute__((visibility("default")))
#else
#  define SKL_WCAS_API
#endif

#include <type_traits>

namespace skl::wcas::detail::bitmask {
template<typename E>
struct EnableBitmask : std::false_type {};
}   // namespace skl::wcas::detail::bitmask

#define SKL_WCAS_ENABLE_BITMASK_OPERATORS(E) \
    template<>                               \
    struct skl::wcas::detail::bitmask::EnableBitmask<E> : std::true_type {}

template<typename E>
constexpr E operator|(E lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) | static_cast<U>(rhs));
}

template<typename E>
constexpr E operator&(E lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) & static_cast<U>(rhs));
}

template<typename E>
constexpr E operator^(E lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(lhs) ^ static_cast<U>(rhs));
}

// 单目：返回类型 SFINAE 同样有效
template<typename E>
constexpr E operator~(E e) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    using U = std::underlying_type_t<E>;
    return static_cast<E>(~static_cast<U>(e));
}

// 复合赋值返回引用
template<typename E>
constexpr E &operator|=(E &lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    return lhs = lhs | rhs;
}

template<typename E>
constexpr E &operator&=(E &lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    return lhs = lhs & rhs;
}

template<typename E>
constexpr E &operator^=(E &lhs, E rhs) noexcept
    requires skl::wcas::detail::bitmask::EnableBitmask<E>::value
{
    return lhs = lhs ^ rhs;
}

template<typename E>
constexpr bool has_flag(E value, E flag) noexcept {
    return (value & flag) == flag;
}

template<typename E>
constexpr bool any_flag(E value, E flag) noexcept {
    return (value & flag) != E{};
}
