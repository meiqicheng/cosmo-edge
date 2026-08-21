# Axera AX650 hardware media SDK (msp/out) selection.
#
# AX650N SoC mode ships an MPP-style media stack under msp/out:
#   libax_sys.so (system init / VB pool), libax_comm.so (common),
#   libax_vdec.so (H264/H265/JPEG decode), libax_venc.so (encode),
#   libax_ivps.so (scaling / color-conversion / crop).
# The SDK root is shared with the inference SDK (COSMO_AXERA_ROOT), so this
# file only adds the media IMPORTED targets and the runtime install.
if(COSMO_MEDIA_USE_AXERA_BACKEND)
    if(NOT COSMO_AXERA_ROOT)
        message(FATAL_ERROR "COSMO_MEDIA_USE_AXERA_BACKEND requires COSMO_AXERA_ROOT "
            "(AX650 SDK msp/out directory)")
    endif()

    set(AXERA_MEDIA_INCLUDE_DIR "${COSMO_AXERA_ROOT}/include")
    set(AXERA_MEDIA_LIB_DIR "${COSMO_AXERA_ROOT}/lib")

    # ax_sys is already defined by cmake/axera.cmake (inference SDK selection).
    foreach(_axlib IN ITEMS
            ax_comm ax_vdec ax_venc ax_ivps)
        if(NOT EXISTS "${AXERA_MEDIA_LIB_DIR}/lib${_axlib}.so")
            message(FATAL_ERROR "AX650 media runtime not found: lib${_axlib}.so")
        endif()
        add_library(${_axlib} SHARED IMPORTED GLOBAL)
        set_target_properties(${_axlib} PROPERTIES
            IMPORTED_LOCATION "${AXERA_MEDIA_LIB_DIR}/lib${_axlib}.so"
            INTERFACE_INCLUDE_DIRECTORIES "${AXERA_MEDIA_INCLUDE_DIR}"
        )
    endforeach()

    # Runtime libraries are bundled with the inference SDK install block in
    # cmake/axera.cmake (lib/ directory install covers both families).
endif()
