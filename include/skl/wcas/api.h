#pragma once
#if defined(_WIN32)
#  if defined(WCAS_BUILDING_LIBRARY)
#    define WCAS_API __declspec(dllexport)
#  else
#    define WCAS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define WCAS_API __attribute__((visibility("default")))
#else
#  define WCAS_API
#endif
