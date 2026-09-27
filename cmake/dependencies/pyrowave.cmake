#
# Loads the optional PyroWave encoder library (pyrowave-shared)
#
# Set PYROWAVE_ROOT to the install prefix of a pyrowave-shared build if it isn't on the default search paths.
#
include_guard(GLOBAL)

set(SUNSHINE_BUILD_PYROWAVE OFF)

if(SUNSHINE_ENABLE_PYROWAVE)
    find_path(PYROWAVE_INCLUDE_DIR pyrowave.h
            HINTS "${PYROWAVE_ROOT}/include"
            PATH_SUFFIXES pyrowave)
    find_library(PYROWAVE_LIBRARY
            NAMES pyrowave-shared
            HINTS "${PYROWAVE_ROOT}/lib")

    # pyrowave.h requires the Vulkan headers to be included first
    find_path(PYROWAVE_VULKAN_INCLUDE_DIR vulkan/vulkan_core.h
            HINTS "${VULKAN_HEADERS_DIR}" "${PYROWAVE_ROOT}/include")

    if(PYROWAVE_INCLUDE_DIR AND PYROWAVE_LIBRARY AND PYROWAVE_VULKAN_INCLUDE_DIR)
        message(STATUS "PyroWave: ${PYROWAVE_LIBRARY}")
        set(SUNSHINE_BUILD_PYROWAVE ON)

        if(WIN32)
            # The DLL is installed next to sunshine.exe
            find_file(PYROWAVE_DLL
                    NAMES libpyrowave-shared-0.dll libpyrowave-shared.dll pyrowave-shared-0.dll pyrowave-shared.dll
                    HINTS "${PYROWAVE_ROOT}/bin")
            if(NOT PYROWAVE_DLL)
                message(FATAL_ERROR "PyroWave DLL not found, set PYROWAVE_ROOT")
            endif()
        endif()
    else()
        message(WARNING "PyroWave (pyrowave-shared and Vulkan headers) not found, the PyroWave encoder is disabled")
    endif()
endif()
