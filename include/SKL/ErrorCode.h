#pragma once
#include "SKL/Config.h"
#include <system_error>
#include <boost/outcome.hpp>

#ifndef SKL_ERROR_MACROS
#  define SKL_ERROR_MACROS
#  define SKL_STDERR_FALLBACK {ev, *this}
#  define SKL_ERR_X_ENUM(name, msg, val, map) name = (val),
#  define SKL_ERR_X_MSG(name, msg, val, map) \
      case SKL_ERR_ENUM::name: return msg;
#  define SKL_ERR_X_MAP(name, msg, val, map) \
      case SKL_ERR_ENUM::name: return map;
#endif

namespace skl {
class error_category : public std::error_category {
public:
    ~error_category() override = default;
    [[nodiscard]] const char *name() const noexcept override = 0;
    [[nodiscard]] std::string message(int ev) const override = 0;
    [[nodiscard]] virtual const char *module() const noexcept = 0;
    [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override = 0;

protected:
    error_category() = default;
};
}   // namespace skl

#define SKL_DERIVE_ERROR_H(MODULEID, XMID)                                                          \
    namespace MODULEID {                                                                            \
    enum class errc {                                                                               \
        SKL_##XMID##_ERROR_CODES(SKL_ERR_X_ENUM)                                                    \
    };                                                                                              \
    class error_category : public skl::error_category {                                             \
    public:                                                                                         \
        ~error_category() = default;                                                                \
        SKL_INSTANCE(error_category)                                                                \
        [[nodiscard]] const char *name() const noexcept override;                                   \
        [[nodiscard]] std::string message(int ev) const override;                                   \
        [[nodiscard]] const char *module() const noexcept override;                                 \
        [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override; \
                                                                                                    \
    private:                                                                                        \
        error_category() = default;                                                                 \
    };                                                                                              \
    }                                                                                               \
    std::error_code make_error_code(MODULEID::errc ec);

#define SKL_DERIVE_ERROR_CPP(MODULEID, ec_macros)                                                               \
    namespace MODULEID {                                                                                        \
    const char *error_category::name() const noexcept { return "skl." #MODULEID; }                              \
    std::string error_category::message(int ev) const {                                                         \
        std::error_code ec;                                                                                     \
        switch ((errc)ev) { ec_macros(SKL_ERR_X_MSG) default : return "Invalid error, illegal out-of-bounds"; } \
    }                                                                                                           \
    const char *error_category::module() const noexcept { return #MODULEID; }                                   \
    std::error_condition error_category::default_error_condition(int ev) const noexcept {                       \
        if (!ev) return {0, std::generic_category()};                                                           \
        switch ((errc)ev) { ec_macros(SKL_ERR_X_MAP) default : return {ev, *this}; }                            \
    }                                                                                                           \
    }                                                                                                           \
    std::error_code make_error_code(MODULEID::errc ec) { return {(int)ec, MODULEID::error_category::instance()}; }
