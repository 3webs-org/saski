# Freestanding x86_64 Mach-O toolchain. Selected via:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake
#
# Only the compiler side lives here. The final link runs through the clang
# driver with --ld-path (see LD in the top-level CMakeLists.txt) rather than
# the system default linker, since this is a non-hosted Mach-O with no
# libSystem/CRT.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-18)
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()

set(CMAKE_C_COMPILER_TARGET   x86_64-apple-darwin)
set(CMAKE_ASM_COMPILER_TARGET x86_64-apple-darwin)

# -nostdinc + the three-header include/freestanding/ (stdint.h, stddef.h,
# stdarg.h): this build needs nothing else from a system include path, and
# not depending on the ambient compiler's own bundled headers being present
# or correct is deliberate -- see include/freestanding/stdint.h's own note.
#
# -fpie: required, not optional, for this architecture -- the kernel's own
# load-address and SMP/relocation design (see smp_layout.h, kernel.c's
# kernel_boot) assumes RIP-relative/PIE codegen throughout. aarch64 is the
# one exception (see toolchain-aarch64.cmake for why).
#
# The rest are ABI facts (freestanding ABI, no red zone/SSE/MMX for a
# kernel that may run before FPU state is set up), not optimization
# choices, so they live here rather than being left to the caller.
set(CMAKE_C_FLAGS_INIT
    "-nostdinc -isystem ${CMAKE_CURRENT_LIST_DIR}/../include/freestanding -std=c11 -ffreestanding -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra")
