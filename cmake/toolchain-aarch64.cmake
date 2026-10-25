# Freestanding aarch64 ELF toolchain. Selected via:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
#
# Only the compiler side lives here. The final link runs through the clang
# driver with --ld-path pointing at ld.lld (an ELF linker -- NOT ld64.lld,
# which is Mach-O only; see LD in the top-level CMakeLists.txt).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-18)
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()

set(CMAKE_C_COMPILER_TARGET   aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)

# -nostdinc + the three-header include/freestanding/ (stdint.h, stddef.h,
# stdarg.h): see toolchain-x86_64.cmake's identical note.
#
# -fno-pic, NOT -fpie, deliberately: an AP runs a raw copy of this image at
# an address assigned at runtime, with no dyld present to rebase GOT slots
# the way -fpie codegen would require. -fno-pic emits PC-relative
# ADRP/ADD references to local symbols instead, which a raw copy still
# resolves correctly. The BSP's own entry assembly can still use absolute
# literals, since APs enter kernel_boot_ap directly rather than through it.
#
# -mgeneral-regs-only: a freestanding kernel must not depend on the FPU
# being enabled -- firmware may leave it off, and touching an FP register
# with it off is an exception with no handler.
set(CMAKE_C_FLAGS_INIT
    "-nostdinc -isystem ${CMAKE_CURRENT_LIST_DIR}/../include/freestanding -std=c11 -ffreestanding -fno-stack-protector -fno-pic -mgeneral-regs-only -Wall -Wextra")
