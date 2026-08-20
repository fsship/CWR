set(CMAKE_SYSTEM_NAME Windows)

# vcpkg configures ports in a separate CMake process and does not preserve
# cache values from the parent configure.  Prefer the repository's portable
# LLVM bundle when it exists, so that the same chainload toolchain works for
# both the game and vcpkg's compiler probe.
if(NOT POSEIDON_LLVM_ROOT AND NOT "$ENV{POSEIDON_LLVM_ROOT}")
    get_filename_component(_poseidon_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(_poseidon_portable_llvm "${_poseidon_source_root}/build/tools/LLVM")
    if(EXISTS "${_poseidon_portable_llvm}/bin/clang-cl.exe")
        set(POSEIDON_LLVM_ROOT "${_poseidon_portable_llvm}")
    endif()
endif()

# Find LLVM compilers dynamically
include(${CMAKE_CURRENT_LIST_DIR}/../FindLLVMSanitizer.cmake)
find_llvm_compilers("x64")

# vcpkg invokes this chainload toolchain in separate CMake processes.  Those
# processes cannot rely on the parent VS environment to discover mt.exe, so
# explicitly use the manifest tool shipped alongside our portable LLVM.
set(_poseidon_llvm_mt "${POSEIDON_LLVM_ROOT}/bin/llvm-mt.exe")
if(EXISTS "${_poseidon_llvm_mt}")
    set(CMAKE_MT "${_poseidon_llvm_mt}" CACHE FILEPATH "Manifest tool" FORCE)
endif()

# 64 bit
set(CMAKE_C_FLAGS_INIT   "-m64")
set(CMAKE_CXX_FLAGS_INIT "-m64")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "/machine:x64")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/machine:x64")
set(CMAKE_STATIC_LINKER_FLAGS_INIT "/machine:x64")
