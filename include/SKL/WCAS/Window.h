#ifndef SKL_WCAS_WINDOW_H
#define SKL_WCAS_WINDOW_H

#include "SKL/WCAS/API.h"
#include "SKL/WCAS/DRM.h"
#include "SKL/WCAS/Types.h"
#include "SKL/WCAS/WCAS.h"

#include "SKL/ABIX/Interface/Declaration.h"

#include <string>
#include <vector>

namespace skl::wcas {

// Window enumeration and geometry resolution are separate capabilities:
//   * an enumerator lists the windows a transport/session can see;
//   * a resolver maps a pid or a size/title pair to a compositor rectangle.
//
// The interface is process-internal for now: WindowInfo contains std::string
// and the list is returned by value. A module-facing version should use a POD
// WindowDescriptor instead.
#define SKL_WCAS_WINDOW_ENUMERATOR_METHODS(X) \
    X(name, std::string, str, {})             \
    X(connected, bool, bool_t, {})            \
    X(list, result<std::vector<WindowInfo>>, reg_window::list, {})

#define SKL_WCAS_WINDOW_RESOLVER_METHODS(X)               \
    X(name, std::string, str, {})                         \
    X(connected, bool, bool_t, {})                        \
    X(find_by_pid, result<cv::Rect>, reg_cv::rect, {i32}) \
    X(find_by_size_and_title, result<cv::Rect>, reg_cv::rect, {i32, i32, str})

SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    WindowEnumerator, SKL_WCAS_WINDOW_ENUMERATOR_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);
SKL_ABIX_INTERFACE_UNIQUE_DECLARE(
    WindowResolver, SKL_WCAS_WINDOW_RESOLVER_METHODS, SKL_ABIX_VERSION_NUMBER(0, 1, 0), SKL_WCAS_API);

#undef SKL_WCAS_WINDOW_ENUMERATOR_METHODS
#undef SKL_WCAS_WINDOW_RESOLVER_METHODS

}   // namespace skl::wcas

#endif   // SKL_WCAS_WINDOW_H
