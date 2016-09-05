if (SPEEX_INCLUDE_DIR)
	set (SPEEX_FIND_QUIETLY TRUE)
endif ()

find_package (PkgConfig)
pkg_check_modules(PC_SPEEX QUIET speex)

find_path (SPEEX_INCLUDE_DIR speex/speex.h HINTS ${PC_SPEEX_INCLUDEDIR} ${PC_SPEEX_INCLUDE_DIRS})
find_library (SPEEX_LIBRARY NAMES speex HINTS ${PC_SPEEX_LIBDIR} ${PC_SPEEX_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SPEEX DEFAULT_MSG SPEEX_INCLUDE_DIR SPEEX_LIBRARY)

mark_as_advanced (SPEEX_INCLUDE_DIR SPEEX_LIBRARY)

set(SPEEX_INCLUDE_DIRS ${SPEEX_INCLUDE_DIR})
set(SPEEX_LIBRARIES ${SPEEX_LIBRARY})

