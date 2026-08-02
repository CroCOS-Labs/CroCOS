#TODO enable SSE and SSE2, and remove -mno-sse(2)
set(CMAKE_ASM_FLAGS "-ffreestanding -mcmodel=kernel  -fno-exceptions -nostdlib -Wall -Wextra -x assembler-with-cpp -masm=att -DCR_BOOT")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fimplicit-constexpr -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -fno-sized-deallocation -fconcepts -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel -masm=att -fconcepts-diagnostics-depth=3 -fstack-protector-strong -DKERNEL -DCR_BOOT -Wno-vla")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-use-cxa-atexit -mno-sse -mno-sse2 -mno-avx -ffreestanding -fno-rtti -fno-exceptions -nostdlib -Wall -Wextra -pedantic -Wshadow -Wcast-align -Wwrite-strings -Wredundant-decls -Winline -Wno-long-long -Wconversion -Werror -mno-red-zone -mcmodel=kernel -masm=att -fstack-protector-strong -DCR_BOOT -Wno-vla")

# Assert the kernel links the libgcc built for its own ABI.
#
# The toolchain provides two (see tools/toolchain/README.md): a PIC one for
# eventual userspace, and a -mcmodel=kernel -mno-red-zone one for us. GCC picks
# between them purely by matching the multilib spec against the flags above, and
# a mismatch is silent -- the kernel would just link the small-model, red-zone,
# PIC libgcc and keep working right up until it doesn't. Ask the compiler which
# one the real CMAKE_CXX_FLAGS actually select, so dropping either flag above is
# a configure-time error rather than a bug discovered years later.
separate_arguments(_crocos_kernel_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} ${_crocos_kernel_flags} -print-libgcc-file-name
        OUTPUT_VARIABLE _crocos_kernel_libgcc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _crocos_kernel_libgcc_result
        ERROR_VARIABLE _crocos_kernel_libgcc_error
)
if(NOT _crocos_kernel_libgcc_result EQUAL 0)
    message(FATAL_ERROR
            "Could not determine which libgcc the kernel flags select.\n"
            "  compiler: ${CMAKE_CXX_COMPILER}\n"
            "  error:    ${_crocos_kernel_libgcc_error}")
endif()
if(NOT _crocos_kernel_libgcc MATCHES "/kernel/")
    message(FATAL_ERROR
            "The kernel's flags select the wrong libgcc multilib.\n"
            "  selected: ${_crocos_kernel_libgcc}\n"
            "  expected: a path under .../kernel/no-red-zone/\n"
            "This means -mcmodel=kernel and/or -mno-red-zone is missing from "
            "CMAKE_CXX_FLAGS above, or the toolchain was built without the "
            "multilib in gcc/config/i386/t-x86_64-crocos. Rebuild it with "
            "tools/toolchain/build-toolchain.sh.")
endif()
message(STATUS "Kernel libgcc: ${_crocos_kernel_libgcc}")

set(KERNEL_SRC
        arch/amd64/amd64.cpp
        arch/amd64/InstructionWrappers.cpp
        mm/PageAllocator.cpp
        arch/amd64/SerialPort.cpp
        arch/amd64/multiboot1.asm
        arch/amd64/interrupts/InterruptVectorTrampolines.asm
        KernelMain.cpp
        arch/arch.cpp
        panic.cpp
        mm/kmalloc.cpp
        acpi/ACPIFixedTables.cpp
        mm/MemoryManager.cpp
        timing/TimerQueues.cpp
        timing/ClockManager.cpp
        arch/amd64/interrupts/LegacyPIC.cpp
        arch/amd64/smp/smp.cpp
        arch/amd64/interrupts/InterruptSetup.cpp
        arch/amd64/lgdt.asm
        interrupts/InterruptGraphManager.cpp
        cxxcompat.cpp
        arch/amd64/interrupts/APIC.cpp
        arch/amd64/timers/PIT.cpp
        arch/amd64/timers/HPET.cpp
        interrupts/InterruptRoutingAndDispatch.cpp
        interrupts/InterruptRoutingPolicyImpls.cpp
        interrupts/InterruptContextDepths.cpp
        interrupts/ExplicitTypes.cpp
        timing/BlockingSleep.cpp
        arch/amd64/smp/smpboot.asm
        init.cpp
        mm/MemTypes.cpp
        mm/NUMA.cpp
        acpi/NUMAIterators.cpp
        mm/VMSubstrate.cpp
        mm/VMSubstrateSlab.cpp
        mm/vmsmalloc.cpp
        RcuStress.cpp
        rcu/RCU.cpp
        CpuLocal.cpp
        arch/amd64/CpuLocalBase.cpp
)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated_headers)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated_source)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated_other)

set_source_files_properties(
        InterruptGraphManager.cpp
        InterruptRoutingAndDispatch.cpp
        interrupts/InterruptRoutingPolicyImpls.cpp
        mm/VMSubstrate.cpp
        mm/VMSubstrateSlab.cpp
        mm/vmsmalloc.cpp
        RcuStress.cpp
        PROPERTIES COMPILE_FLAGS "-O2 -ftemplate-backtrace-limit=0"
)

#We need to be smarter to split out architecture-specific source files
file(GLOB_RECURSE COMMON_INIT_FILES
        "${CMAKE_SOURCE_DIR}/kernel/*.icd"
)

set(INIT_DSL_FILES ${COMMON_INIT_FILES})

# Define output location
set(GENERATED_INIT_CPP "${CMAKE_BINARY_DIR}/generated_source/init_registry.cpp")

# Custom command to generate the header
add_custom_command(
        OUTPUT ${GENERATED_INIT_CPP}
        COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/gen_init_registry.py"
        -o ${GENERATED_INIT_CPP}
        ${INIT_DSL_FILES}
        DEPENDS
        ${INIT_DSL_FILES}
        "${CMAKE_SOURCE_DIR}/tools/gen_init_registry.py"
        "${CMAKE_SOURCE_DIR}/tools/toollib.py"  # If you have helper modules
        COMMENT "Generating initialization registry from ICD files"
        VERBATIM
)

set(LINKER_SCRIPT_TEMPLATE "${CMAKE_SOURCE_DIR}/kernel/arch/amd64/linker.ld")
set(GENERATED_LINKER_SCRIPT "${CMAKE_BINARY_DIR}/generated_other/linker.ld")
set(GENERATED_BASE_HEADER "${CMAKE_BINARY_DIR}/generated_headers/kbase.h")

add_custom_command(
        OUTPUT ${GENERATED_LINKER_SCRIPT} ${GENERATED_BASE_HEADER}
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/DeriveKernelLayout.cpp"
        DEPENDS "${LINKER_SCRIPT_TEMPLATE}"
        generate_linker_script
        COMMAND ${CMAKE_BINARY_DIR}/tools/generate_linker_script ${LINKER_SCRIPT_TEMPLATE} ${GENERATED_LINKER_SCRIPT} ${GENERATED_BASE_HEADER}
)

add_custom_target(create_linker_script
    DEPENDS ${GENERATED_LINKER_SCRIPT} ${GENERATED_BASE_HEADER}
)

# Create a custom target for the generated header
add_custom_target(generate_init_registry
        DEPENDS ${GENERATED_INIT_HEADER}
)

add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/generated_headers/isr.inc, ${CMAKE_BINARY_DIR}/generated_headers/isr_set.inc
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_idt_install_macros.py
        -o ${CMAKE_BINARY_DIR}/generated_headers/
        DEPENDS ${INIT_DEF_FILES} ${CMAKE_SOURCE_DIR}/tools/gen_idt_install_macros.py
        COMMENT "Generating init registry"
)

add_custom_target(generate_idt_macros
    DEPENDS ${CMAKE_BINARY_DIR}/generated_headers/isr.inc, ${CMAKE_BINARY_DIR}/generated_headers/isr_set.inc
)

# P4-DEC-006 coverage instrumentation. OFF by default: these counters answer
# "is the DEC-047 hazard actually being presented to onPreTouch", which is a
# question you ask of a stress run, not something a shipping kernel should pay
# for on its retire and reclaim paths.
#
# Deliberately makes the stats fields ABSENT rather than zero when off — a build
# that printed "stale=0" would read as a coverage regression under this phase's
# Verification Targets, which is the opposite of the truth.
option(CROCOS_FRESHNESS_STATS "Count RCU freshness hits and slab-page reclaims (P4-DEC-006)" OFF)

add_executable(Kernel ${KERNEL_SRC} ${GENERATED_INIT_CPP})
if(CROCOS_FRESHNESS_STATS)
    target_compile_definitions(Kernel PRIVATE CROCOS_FRESHNESS_STATS)
endif()
add_dependencies(Kernel generate_idt_macros generate_init_registry create_linker_script create_linker_script)
target_compile_options(Kernel PRIVATE -gdwarf-4)
target_compile_options(Kernel PRIVATE -ffunction-sections -fdata-sections)
target_link_options(Kernel PUBLIC
        -flto -Os -ffreestanding -mcmodel=kernel -nostdlib
        -T ${GENERATED_LINKER_SCRIPT}
        -Wl,--gc-sections
        #-Wl,--print-gc-sections
        -Wl,--build-id
)
target_include_directories(Kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(Kernel PRIVATE ${CMAKE_BINARY_DIR}/generated_headers)
# kernel/mm holds vmsmalloc's implementation-internal headers (VMSubstrateSlab.h);
# cpu_local.h needs the Magazine type definition from there per P45-DEC-001.
target_include_directories(Kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mm)
target_link_libraries(Kernel PRIVATE CoreKernel AllocKernel MMIOKernel gcc)

add_custom_command(TARGET Kernel POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:Kernel> $<TARGET_FILE:Kernel>.debug
        COMMAND ${CMAKE_OBJCOPY} --strip-all --add-gnu-debuglink=$<TARGET_FILE:Kernel>.debug $<TARGET_FILE:Kernel>
        COMMENT "Stripping symbols and creating debug file with build ID"
)