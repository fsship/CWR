# OpenVR is distributed upstream as a prebuilt DLL/import-library pair. It is a
# dynamic runtime bridge regardless of the surrounding triplet's default
# linkage; do not change the whole triplet to dynamic just for this port, since
# that invalidates every unrelated dependency's ABI cache.
set(VCPKG_POLICY_DLLS_IN_STATIC_LIBRARY enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ValveSoftware/openvr
    REF "v${VERSION}"
    SHA512 add58746e4ee55ca78c0132bfc1b8d649dc0ea49d57bbffd3707bf6cbca66f387980e3f4508ae027b25bc560ec54e02269f245e8c41b85c2f03226670048833e
    HEAD_REF master
)

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(ARCH_PATH "win64")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(ARCH_PATH "win32")
else()
    message(FATAL_ERROR "OpenVR supports x64 and x86 Windows in this overlay.")
endif()

file(COPY "${SOURCE_PATH}/lib/${ARCH_PATH}/" DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
file(COPY "${SOURCE_PATH}/lib/${ARCH_PATH}/" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
file(COPY "${SOURCE_PATH}/bin/${ARCH_PATH}/" DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
file(COPY "${SOURCE_PATH}/bin/${ARCH_PATH}/" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
file(INSTALL "${SOURCE_PATH}/headers" DESTINATION "${CURRENT_PACKAGES_DIR}" RENAME include)
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
