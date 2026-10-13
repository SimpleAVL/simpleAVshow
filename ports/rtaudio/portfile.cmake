vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO thestk/rtaudio
    REF ${VERSION}
    SHA512 085feb2673185460717ba45fc87254961e477823759e11281092c1ba13301303de1cd36aa9efeba0710cbf2c70f2e2f7f9e41173cf372ded528c41612b19acd5
    HEAD_REF master
    PATCHES
        fix-pulse.patch
)

# SimpleAVShow overlay: RtAudio bundles 2013 copies of Steinberg's ASIO SDK, which were licensed
# only under Steinberg's proprietary agreement. SDK 2.3.4 (2025) is dual-licensed (proprietary or
# GPLv3, with BSD-licensed host helpers), so build from it and keep RtAudio's two small fixes.
set(ASIO_LICENSE "")
if("asio" IN_LIST FEATURES)
    vcpkg_download_distfile(ASIO_ARCHIVE
        URLS "https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip"
        FILENAME "ASIO-SDK_2.3.4_2025-10-15.zip"
        SHA512 57de2c0cd0df0783275987e08255abfa49da12982f9d462ac40b7f57300c36e024dcb65d100b799fb3c96a9c7c5ee86e61ceb0e68d2839324206c1629d3905ed
    )
    vcpkg_extract_source_archive(ASIO_PATH ARCHIVE "${ASIO_ARCHIVE}")
    foreach(pair IN ITEMS common/asio.cpp common/asio.h common/asiodrvr.h common/asiosys.h common/iasiodrv.h
            host/asiodrivers.cpp host/asiodrivers.h host/ginclude.h host/pc/asiolist.h)
        get_filename_component(name "${pair}" NAME)
        file(COPY_FILE "${ASIO_PATH}/${pair}" "${SOURCE_PATH}/include/${name}")
    endforeach()
    file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/asiolist.cpp" "${SOURCE_PATH}/include/asiolist.cpp")
    # RtAudio guards driver removal when no driver list exists.
    file(READ "${SOURCE_PATH}/include/asio.cpp" asio_source)
    string(REPLACE "\t\tasioDrivers->removeCurrentDriver();" "\t\tif (asioDrivers) // RtAudio fix\n\t\t\tasioDrivers->removeCurrentDriver();" patched "${asio_source}")
    if(patched STREQUAL asio_source)
        message(FATAL_ERROR "ASIO SDK asio.cpp no longer matches the RtAudio null-check patch")
    endif()
    file(WRITE "${SOURCE_PATH}/include/asio.cpp" "${patched}")
    file(COPY_FILE "${ASIO_PATH}/LICENSE.txt" "${SOURCE_PATH}/ASIO-SDK-LICENSE.txt")
    set(ASIO_LICENSE "${SOURCE_PATH}/ASIO-SDK-LICENSE.txt")
endif()

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" RTAUDIO_STATIC_MSVCRT)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        asio  RTAUDIO_API_ASIO
        alsa  RTAUDIO_API_ALSA
        pulse RTAUDIO_API_PULSE
)
set(PKG_OPT "")
if("pulse" IN_LIST FEATURES)
    vcpkg_find_acquire_program(PKGCONFIG)
    set(PKG_OPT "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}")
endif()
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRTAUDIO_STATIC_MSVCRT=${RTAUDIO_STATIC_MSVCRT}
        -DRTAUDIO_API_JACK=OFF
        ${FEATURE_OPTIONS}
        ${PKG_OPT}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" ${ASIO_LICENSE})
