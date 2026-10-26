# saski build.
#
# Usage:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake -DLD64=/path/to/ld64.lld
#   cmake --build build
#
# (toolchain-aarch64.cmake for the other architecture.)
#
# TWO INDEPENDENT BUILD AXES. Do not couple them.
# - CMAKE_SYSTEM_PROCESSOR (set by the toolchain file) selects the
#   architecture: which sources are built, the boot path, and the link
#   convention (x86_64 links Mach-O via ld64.lld; aarch64 links ELF via
#   ld.lld -- genuinely different tools, not just different flags).
# - EMBEDDED selects whether a device tree a platform would otherwise be
#   HANDED at boot is instead compiled into the image. Orthogonal to
#   architecture by design: a target whose firmware hands over a good DTB
#   has no reason to carry its own, independent of instruction set. Default
#   OFF for every arch, so an unset EMBEDDED is invisible to the build.
#
# No CMAKE_BUILD_TYPE is set by this file: see the note below.

cmake_minimum_required(VERSION 3.21)
project(saski C ASM)

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "note: no CMAKE_BUILD_TYPE is set, so this build applies no optimization level or debug info.")
    message(STATUS "      -DCMAKE_BUILD_TYPE=MinSizeRel is a reasonable default for this freestanding image (-Os).")
    message(STATUS "      -DCMAKE_BUILD_TYPE=Debug adds -g instead.")
endif()

option(EMBEDDED "Compile a device tree blob into the image instead of trusting firmware to supply one" OFF)
set(DTS_SOURCE "" CACHE FILEPATH "Device tree source to embed when EMBEDDED is ON (tests/qemu-virt.dts is a test fixture)")

set(PYTHON3 "python3" CACHE STRING "Python 3 interpreter used for build-time code generation")
set(DTC "dtc" CACHE STRING "Device tree compiler, used only when EMBEDDED is ON")
set(OBJCOPY "llvm-objcopy" CACHE STRING "objcopy, used only for the aarch64 ELF-to-binary step")

# LD64 anchors the toolchain location. It is this build's own x86_64
# linker, AND it is always needed even on an aarch64 build, to build the
# sibling otos-coreinit init image below (that repo's own Mach-O always
# links via ld64.lld, regardless of THIS build's architecture).
set(LD64 "ld64.lld" CACHE STRING "Path to ld64.lld (or a compatible Mach-O linker)")
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # aarch64 links ELF, which ld64.lld cannot do. ld.lld (the ELF linker)
    # is resolved next to LD64 by default -- same convention build.sh used
    # to use via its own LDELFD, since one toolchain location then covers
    # both linkers.
    get_filename_component(LD64_DIR "${LD64}" DIRECTORY)
    if(LD64_DIR STREQUAL "")
        set(LD_DEFAULT "ld.lld")
    else()
        set(LD_DEFAULT "${LD64_DIR}/ld.lld")
    endif()
    set(LD "${LD_DEFAULT}" CACHE STRING "Path to ld.lld (the ELF linker; ld64.lld does not do ELF)")
else()
    set(LD "${LD64}" CACHE STRING "Path to the linker for this architecture")
endif()

# Architecture-specific configuration: which sources, the boot path, and
# the facts the final link needs.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(BOOT_SOURCE kernel/boot.S)
    set(SERIAL_SOURCE kernel/serial_x86_64.c)
    set(ARCH_SOURCES "")
    set(SASKI_LINK_OPTIONS
        --ld-path=${LD} -nostdlib
        -Wl,-arch,x86_64 -Wl,-platform_version,macos,10.12,10.12
        # 0x1000000 (16 MiB), matching include/smp_layout.h's own
        # KERNEL_LOAD_BASE -- kernel.c's boot_info.kernel_load_base needs
        # the two to agree, or SMP bring-up's copy-to-an-AP's-region delta
        # would be silently wrong. This kernel used to link (and load) at
        # address 0, which directly collided with firmware-owned low
        # memory (confirmed with GDB: OVMF places the ACPI RSDP anywhere
        # from 0x2469b to 0x777e000 depending on VM RAM size) -- a real
        # __PAGEZERO of this size pushes __TEXT's vmaddr safely clear of
        # every firmware-owned low-memory structure actually observed.
        -Wl,-pagezero_size,0x1000000 -Wl,-e,_start
    )
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(BOOT_SOURCE arch/boot_aarch64.S)
    set(SERIAL_SOURCE kernel/serial_aarch64.c)
    set(ARCH_SOURCES kernel/kernel_boot_aarch64.c lib/fdt.c)
    # The aarch64 kernel's single link address, as an offset from QEMU
    # virt's RAM base. 16 MiB in: clear of the 1 MiB firmware-supplied
    # device tree AAVMF places at the bottom of RAM (GRUB's xnu loader
    # allocates the kernel's own range from EFI, so it must be free), and
    # clear of the reference init's declared vmaddr (otos-coreinit,
    # 0x40200000). Every aarch64 boot path -- QEMU -kernel (via the Image
    # header's text_offset), GRUB xnu_kernel64 (via the Mach-O vmaddr),
    # the EFI stub (via its copy destination) -- derives from this value.
    set(AARCH64_RAM_BASE "0x40000000" CACHE STRING
        "Physical RAM base used by the arm64 Image-header direct boot path")
    math(EXPR AARCH64_LOAD_BASE "${AARCH64_RAM_BASE} + 0x1000000" OUTPUT_FORMAT HEXADECIMAL)
    set(SASKI_LINK_OPTIONS
        --ld-path=${LD} -nostdlib
        -Wl,-e,_start -Wl,-Ttext,${AARCH64_LOAD_BASE}
        # AArch64 ELF's default 64 KiB segment alignment puts 128 KiB of
        # zero-filled gaps between .text, .rodata and .data in the flat
        # Image. QEMU virt uses 4 KiB pages; 4 KiB segment alignment keeps
        # the same load addresses while eliminating 120 KiB of file gaps.
        -Wl,-z,max-page-size=4096
    )
else()
    message(FATAL_ERROR
        "Unknown CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'. Supported: x86_64, aarch64\n"
        "Select one with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-<arch>.cmake")
endif()

set(COMMON_SOURCES
    kernel/kernel.c
    kernel/serial.c
    kernel/cap.c
    kernel/capnp_validate.c
    kernel/macho.c
    lib/freestanding_mem.c
)

# The device tree, when this build is asked to carry one. Gated on
# EMBEDDED alone -- not on architecture -- because whether a kernel trusts
# a firmware-supplied tree or carries its own is a deployment decision,
# not a property of the instruction set. See the axes note at the top.
set(DT_SOURCES "")
if(EMBEDDED)
    if(DTS_SOURCE STREQUAL "")
        message(FATAL_ERROR "EMBEDDED is ON but DTS_SOURCE is not set (tests/qemu-virt.dts is a test fixture)")
    endif()
    if(IS_ABSOLUTE "${DTS_SOURCE}")
        set(DTS_SOURCE_PATH "${DTS_SOURCE}")
    else()
        set(DTS_SOURCE_PATH "${CMAKE_SOURCE_DIR}/${DTS_SOURCE}")
    endif()
    if(NOT EXISTS "${DTS_SOURCE_PATH}")
        message(FATAL_ERROR "no device-tree source at ${DTS_SOURCE_PATH}")
    endif()
    set(SASKI_DTB "${CMAKE_BINARY_DIR}/saski.dtb")
    add_custom_command(
        OUTPUT "${SASKI_DTB}"
        COMMAND "${DTC}" -I dts -O dtb -o "${SASKI_DTB}" "${DTS_SOURCE_PATH}"
        DEPENDS "${DTS_SOURCE_PATH}"
        COMMENT "Compiling device tree ${DTS_SOURCE_PATH}"
    )
    set(DT_SOURCES arch/dt_blob.S)
    set_source_files_properties(arch/dt_blob.S PROPERTIES
        COMPILE_DEFINITIONS "DTB_PATH=\"${SASKI_DTB}\""
        OBJECT_DEPENDS "${SASKI_DTB}"
    )
endif()

# The userspace init image, embedded directly into the kernel binary (see
# boot.S's own .incbin), not discovered as a separate module. INIT_MACHO is
# a plain build-time choice, deliberately not something the kernel itself
# has any opinion about. If not given directly, the sibling otos-coreinit
# repository is built automatically, matching THIS build's own
# architecture -- see include/init_abi.h's own note on why coreinit owns
# this choice.
if(DEFINED ENV{INIT_MACHO})
    set(INIT_MACHO_DEFAULT "$ENV{INIT_MACHO}")
else()
    set(INIT_MACHO_DEFAULT "")
endif()
set(INIT_MACHO "${INIT_MACHO_DEFAULT}" CACHE FILEPATH
    "Path to the userspace init Mach-O to embed. Empty builds otos-coreinit (COREINIT_ROOT) automatically.")

if(INIT_MACHO STREQUAL "")
    if(DEFINED ENV{COREINIT_ROOT})
        set(COREINIT_ROOT_DEFAULT "$ENV{COREINIT_ROOT}")
    else()
        set(COREINIT_ROOT_DEFAULT "${CMAKE_SOURCE_DIR}/../otos-coreinit")
    endif()
    set(COREINIT_ROOT "${COREINIT_ROOT_DEFAULT}" CACHE PATH
        "Root of the otos-coreinit repository (built automatically for INIT_MACHO unless INIT_MACHO is set directly)")
    # This message is the first thing a newcomer hits when the sibling repo
    # sits somewhere other than the default or is absent, so it names the
    # variable and the path actually tried; it is deliberately symmetric
    # with otos-coreinit's own SASKI_ROOT error, so the cross-repo handshake
    # reads as one convention rather than two dialects.
    if(NOT EXISTS "${COREINIT_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "COREINIT_ROOT not found -- no CMakeLists.txt at ${COREINIT_ROOT}/CMakeLists.txt.\n"
            "Set -DCOREINIT_ROOT=... to the otos-coreinit repository root (default: ${CMAKE_SOURCE_DIR}/../otos-coreinit), or -DINIT_MACHO=... directly.")
    endif()
    set(COREINIT_BUILD "${CMAKE_BINARY_DIR}/coreinit")
    set(COREINIT_MACHO "${COREINIT_BUILD}/coreinit.macho")
    add_custom_command(
        OUTPUT "${COREINIT_MACHO}"
        COMMAND "${CMAKE_COMMAND}" -S "${COREINIT_ROOT}" -B "${COREINIT_BUILD}"
                -G "${CMAKE_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                "-DCMAKE_TOOLCHAIN_FILE=${COREINIT_ROOT}/cmake/toolchain-${CMAKE_SYSTEM_PROCESSOR}.cmake"
                "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DLD64=${LD64}" "-DSASKI_ROOT=${CMAKE_SOURCE_DIR}"
        COMMAND "${CMAKE_COMMAND}" --build "${COREINIT_BUILD}"
        COMMENT "Building otos-coreinit (${CMAKE_SYSTEM_PROCESSOR}) for INIT_MACHO"
        VERBATIM
    )
    set(INIT_MACHO "${COREINIT_MACHO}")
endif()

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    # .incbin must see exactly the bytes the judge later checks. Within
    # this build's own graph the dependency edge above already orders
    # things correctly, but INIT_MACHO can also be supplied directly
    # (bypassing that graph entirely) while something else is actively
    # rewriting it -- snapshot once rather than embedding a moving file
    # and judging a different image. tools/qemu-boot-aarch64.sh reads this
    # same snapshot to verify what was actually embedded.
    set(EMBEDDED_INIT_MACHO "${CMAKE_BINARY_DIR}/embedded-init.macho")
    add_custom_command(
        OUTPUT "${EMBEDDED_INIT_MACHO}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${INIT_MACHO}" "${EMBEDDED_INIT_MACHO}"
        DEPENDS "${INIT_MACHO}"
        COMMENT "Snapshotting init image for aarch64 embedding"
    )
    set(BOOT_INIT_MACHO "${EMBEDDED_INIT_MACHO}")
else()
    set(BOOT_INIT_MACHO "${INIT_MACHO}")
endif()

set_source_files_properties("${BOOT_SOURCE}" PROPERTIES
    COMPILE_DEFINITIONS "INIT_MACHO_PATH=\"${BOOT_INIT_MACHO}\""
    OBJECT_DEPENDS "${BOOT_INIT_MACHO}"
)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set_property(SOURCE "${BOOT_SOURCE}" APPEND PROPERTY COMPILE_DEFINITIONS "KERNEL_TEXT_OFFSET=0x1000000")
endif()

# BOOT_SOURCE listed FIRST: with no linker script, input sections land in
# the output in command-line order, and _start's arm64 Image header (see
# boot_aarch64.S's own top comment) must be the first bytes of the
# extracted flat binary below -- not merely true today, but a property
# this ordering exists to guarantee.
add_executable(saski_raw
    ${BOOT_SOURCE}
    ${COMMON_SOURCES}
    ${ARCH_SOURCES}
    ${SERIAL_SOURCE}
    ${DT_SOURCES}
)
target_include_directories(saski_raw PRIVATE "${CMAKE_SOURCE_DIR}/kernel" "${CMAKE_SOURCE_DIR}/include")
set_target_properties(saski_raw PROPERTIES SUFFIX ".macho" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# --ld-path selects ld64.lld/ld.lld over the system default; -nostdlib
# links nothing implicit (no libSystem/CRT) -- this is a non-hosted image
# with its own entry point, not a normal executable. On x86_64,
# -arch/-platform_version are passed explicitly rather than left to the
# driver's own defaults: clang's Darwin linker-flag synthesis assumes an
# actual macOS SDK/host, which isn't present when cross-linking from Linux.
target_link_options(saski_raw PRIVATE ${SASKI_LINK_OPTIONS})

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    # GRUB's Mach-O loader only understands LC_SEGMENT_64 and
    # LC_UNIXTHREAD; ld64.lld's ordinary output carries load commands GRUB
    # silently ignores and, critically, emits LC_MAIN (which GRUB never
    # reads) instead of LC_UNIXTHREAD. This rewrites the load-commands
    # preamble only -- no segment bytes move. See tools/macho_to_xnu.py's
    # own top-of-file comment for the full story.
    set(SASKI_MACHO "${CMAKE_BINARY_DIR}/saski.macho")
    add_custom_command(
        OUTPUT "${SASKI_MACHO}"
        COMMAND "${PYTHON3}" "${CMAKE_SOURCE_DIR}/tools/macho_to_xnu.py" "$<TARGET_FILE:saski_raw>" "${SASKI_MACHO}"
        DEPENDS saski_raw "${CMAKE_SOURCE_DIR}/tools/macho_to_xnu.py"
        COMMENT "Rewriting saski_raw.macho's load commands for GRUB's xnu loader"
    )
    add_custom_target(saski ALL DEPENDS "${SASKI_MACHO}")
else()
    # The raw ELF link is the artifact for -kernel boots (copied, not
    # transformed: QEMU's -kernel path and the EFI stub below both read it
    # as a flat image, needing no Mach-O-specific rewrite). The EFI stub
    # is the artifact for GRUB/AAVMF boots: GRUB's xnu loader does not
    # exist for arm64 upstream, so this flat image is wrapped in a minimal
    # EFI application that GRUB chainloads instead, via otos-coreinit's
    # own arm64 build of GRUB's xnu loader for the Mach-O alternative
    # below. tools/qemu-boot-aarch64.sh is the judge for this path.
    set(SASKI_MACHO "${CMAKE_BINARY_DIR}/saski.macho")
    add_custom_command(
        OUTPUT "${SASKI_MACHO}"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:saski_raw>" "${SASKI_MACHO}"
        DEPENDS saski_raw
        COMMENT "Copying the raw ELF link as saski.macho"
    )
    set(SASKI_BIN "${CMAKE_BINARY_DIR}/saski.bin")
    add_custom_command(
        OUTPUT "${SASKI_BIN}"
        COMMAND "${OBJCOPY}" -O binary "${SASKI_MACHO}" "${SASKI_BIN}"
        DEPENDS "${SASKI_MACHO}"
        COMMENT "Extracting the flat binary image"
    )
    set(SASKI_EFI "${CMAKE_BINARY_DIR}/saski.efi")
    add_custom_command(
        OUTPUT "${SASKI_EFI}"
        COMMAND "${CMAKE_COMMAND}" -E env "KERNEL_DST=${AARCH64_LOAD_BASE}"
                "${PYTHON3}" "${CMAKE_SOURCE_DIR}/tools/mkstub.py" "${SASKI_BIN}" "${SASKI_EFI}"
        DEPENDS "${SASKI_BIN}" "${CMAKE_SOURCE_DIR}/tools/mkstub.py"
        COMMENT "Wrapping the flat image in a minimal EFI application"
    )
    # The same image as a Mach-O for GRUB's xnu_kernel64 -- the boot
    # convention the x86_64 path uses, now available on arm64 through
    # otos-coreinit's arm64 build of GRUB's own xnu loader. saski.macho
    # stays the flat-copy (for -kernel); saski-xnu.macho is the xnu artifact.
    set(SASKI_XNU_MACHO "${CMAKE_BINARY_DIR}/saski-xnu.macho")
    add_custom_command(
        OUTPUT "${SASKI_XNU_MACHO}"
        COMMAND "${PYTHON3}" "${CMAKE_SOURCE_DIR}/tools/macho_arm64.py" "${SASKI_BIN}" "${AARCH64_LOAD_BASE}" "${SASKI_XNU_MACHO}"
        DEPENDS "${SASKI_BIN}" "${CMAKE_SOURCE_DIR}/tools/macho_arm64.py"
        COMMENT "Wrapping the flat image as a Mach-O for GRUB's xnu loader"
    )
    add_custom_target(saski ALL DEPENDS "${SASKI_MACHO}" "${SASKI_EFI}" "${SASKI_XNU_MACHO}")
endif()

add_custom_command(TARGET saski POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E echo "built ${SASKI_MACHO}"
    COMMAND file "${SASKI_MACHO}"
    VERBATIM
)
