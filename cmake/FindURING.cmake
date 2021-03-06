find_path(URING_INCLUDE_DIR NAMES liburing.h)
find_library(URING_LIBRARIES NAMES uring)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    URING DEFAULT_MSG
    URING_LIBRARIES URING_INCLUDE_DIR)

mark_as_advanced(URING_INCLUDE_DIR URING_LIBRARIES)
