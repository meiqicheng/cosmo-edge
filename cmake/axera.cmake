# Axera AX650 runtime (ax_engine SoC SDK) selection.
#
# The AX650N board runs the SoC-mode SDK (msp/out from the AX650 SDK V3.10.2
# package): ax_engine_api.h + ax_sys_api.h headers and libax_engine.so +
# libax_sys.so. The SDK is kept external to the repository (mirrors the RKNN
# runtime convention); point COSMO_AXERA_ROOT at the SDK's msp/out directory.
set(COSMO_AXERA_ROOT "" CACHE PATH "Axera AX650 SoC SDK root containing include/ and lib/")
if(NOT COSMO_AXERA_ROOT AND DEFINED ENV{AXERA_ROOT})
    set(COSMO_AXERA_ROOT "$ENV{AXERA_ROOT}" CACHE PATH "Axera AX650 SDK root" FORCE)
endif()

if(NOT COSMO_AXERA_ROOT)
    message(FATAL_ERROR "COSMO_AXERA_ROOT is required for the AXERA backend "
        "(point it at the AX650 SDK msp/out directory)")
endif()

set(AXERA_ENGINE_HEADER "${COSMO_AXERA_ROOT}/include/ax_engine_api.h")
set(AXERA_SYS_HEADER "${COSMO_AXERA_ROOT}/include/ax_sys_api.h")
set(AXERA_ENGINE_LIBRARY "${COSMO_AXERA_ROOT}/lib/libax_engine.so")
set(AXERA_SYS_LIBRARY "${COSMO_AXERA_ROOT}/lib/libax_sys.so")
if(NOT EXISTS "${AXERA_ENGINE_HEADER}")
    message(FATAL_ERROR "AXERA engine header not found: ${AXERA_ENGINE_HEADER}")
endif()
if(NOT EXISTS "${AXERA_ENGINE_LIBRARY}")
    message(FATAL_ERROR "AXERA engine runtime not found: ${AXERA_ENGINE_LIBRARY}")
endif()

add_library(ax_engine SHARED IMPORTED GLOBAL)
set_target_properties(ax_engine PROPERTIES
    IMPORTED_LOCATION "${AXERA_ENGINE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${COSMO_AXERA_ROOT}/include"
)

if(EXISTS "${AXERA_SYS_LIBRARY}")
    add_library(ax_sys SHARED IMPORTED GLOBAL)
    set_target_properties(ax_sys PROPERTIES
        IMPORTED_LOCATION "${AXERA_SYS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${COSMO_AXERA_ROOT}/include"
    )
endif()

# Bundle the runtime libraries into the deployment package so the target never
# falls back to an incompatible system library (mirrors the Sophon install).
install(DIRECTORY ${COSMO_AXERA_ROOT}/lib/
    DESTINATION lib
    FILES_MATCHING
        PATTERN "*so*"
)
