# Sophon device SDK (libsophon) selection.
#
# libsophon ships in two release families that are not interchangeable:
#   - 0.4.x  : BM1688 / CV186X (this repository's default, 3rd/libsophon-0.4.11)
#   - 0.5.x  : BM1684 / BM1684X (3rd/libsophon-0.5.1)
# The default SDK is chosen per target chip; override it explicitly with
# -DCOSMO_LIBSOPHON_ROOT=<path> when a different SDK root is required.
if(COSMO_TARGET_CHIP_NORMALIZED STREQUAL "bm1684")
    set(_cosmo_default_sophon_sdk "${CMAKE_SOURCE_DIR}/3rd/libsophon-0.5.1")
else()
    set(_cosmo_default_sophon_sdk "${CMAKE_SOURCE_DIR}/3rd/libsophon-0.4.11")
endif()
set(COSMO_LIBSOPHON_ROOT "${_cosmo_default_sophon_sdk}" CACHE PATH
    "Sophon device SDK root (libsophon); defaults per target chip")

set(DEVICE_ROOT_DIR "${COSMO_LIBSOPHON_ROOT}")
set(DEVICE_HEADERS ${DEVICE_ROOT_DIR}/include)
set(DEVICE_LIB_DIR ${DEVICE_ROOT_DIR}/lib)

set(BMLIB_LIB ${DEVICE_LIB_DIR}/libbmlib.so)
set(BMRT_LIB ${DEVICE_LIB_DIR}/libbmrt.so)
set(BMCV_LIB ${DEVICE_LIB_DIR}/libbmcv.so)
set(BMMODEL_LIB ${DEVICE_LIB_DIR}/libbmmodel.so)

# Video decoder/encoder libraries. libsophon 0.4.x names them bmvd/bmvenc;
# libsophon 0.5.x renamed the host libraries to bmvideo/bmvpuapi while keeping
# the exported bmvpu_dec_*/bmvpu_enc_* symbols unchanged. Keep the logical
# link names bmvd/bmvenc and point them at the library the selected SDK ships.
if(EXISTS "${DEVICE_LIB_DIR}/libbmvd.so")
    set(BMVD_LIB ${DEVICE_LIB_DIR}/libbmvd.so)
else()
    set(BMVD_LIB ${DEVICE_LIB_DIR}/libbmvideo.so)
endif()
if(EXISTS "${DEVICE_LIB_DIR}/libbmvenc.so")
    set(BMVENC_LIB ${DEVICE_LIB_DIR}/libbmvenc.so)
else()
    set(BMVENC_LIB ${DEVICE_LIB_DIR}/libbmvpuapi.so)
endif()

# Video decode API differences across libsophon families:
#   - 0.4.x exposes BmVpuDecLogLevel::BMVPU_DEC_LOG_LEVEL_ERR
#   - 0.5.x renamed it to BMVPU_DEC_LOG_LEVEL_ERROR and marks the header deprecated
# (the symbols themselves are unchanged). Signal the new family to the media layer.
if(EXISTS "${DEVICE_LIB_DIR}/libbmvideo.so")
    add_compile_definitions(COSMO_LIBSOPHON_NEW_VIDEO_API)
    # libsophon 0.5.x changed bm_image_destroy to take bm_image by value
    # (0.4.x took bm_image*). Source files historically gate this exact
    # difference behind COSMO_NN_SOPHON_1684X; define it for the 0.5.x family
    # so those branches select the by-value call.
    add_compile_definitions(COSMO_NN_SOPHON_1684X)
endif()

add_library(bmlib SHARED IMPORTED)
set_target_properties(bmlib PROPERTIES
    IMPORTED_LOCATION ${BMLIB_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

add_library(bmrt SHARED IMPORTED)
set_target_properties(bmrt PROPERTIES
    IMPORTED_LOCATION ${BMRT_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

add_library(bmcv SHARED IMPORTED)
set_target_properties(bmcv PROPERTIES
    IMPORTED_LOCATION ${BMCV_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

add_library(bmmodel SHARED IMPORTED)
set_target_properties(bmmodel PROPERTIES
    IMPORTED_LOCATION ${BMMODEL_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

add_library(bmvd SHARED IMPORTED)
set_target_properties(bmvd PROPERTIES
    IMPORTED_LOCATION ${BMVD_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

add_library(bmvenc SHARED IMPORTED)
set_target_properties(bmvenc PROPERTIES
    IMPORTED_LOCATION ${BMVENC_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${DEVICE_HEADERS}"
)

install(DIRECTORY ${DEVICE_ROOT_DIR}/lib/
    DESTINATION lib
    FILES_MATCHING
        PATTERN "*so*"
)
