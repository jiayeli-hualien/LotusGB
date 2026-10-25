#ifndef LOTUSGB_CALLBACK_EXCEPTION_BOUNDARY_HPP
#define LOTUSGB_CALLBACK_EXCEPTION_BOUNDARY_HPP

#include <exception>
#include <utility>

namespace LOTUSGB::LibretroInternal {

template <typename Callback, typename ErrorHandler>
bool invokeCallbackSafely(bool &faulted, Callback &&callback,
                          ErrorHandler &&errorHandler) noexcept {
    if (faulted)
        return false;

    try {
        std::forward<Callback>(callback)();
        return true;
    } catch (const std::exception &error) {
        faulted = true;
        try {
            std::forward<ErrorHandler>(errorHandler)(error.what());
        } catch (...) {
        }
    } catch (...) {
        faulted = true;
        try {
            std::forward<ErrorHandler>(errorHandler)("unknown C++ exception");
        } catch (...) {
        }
    }
    return false;
}

} // namespace LOTUSGB::LibretroInternal

#endif // LOTUSGB_CALLBACK_EXCEPTION_BOUNDARY_HPP
