# Use Homebrew clang with C++26 support, fallback to system clang
find_program(HOMEBREW_CLANG /opt/homebrew/opt/llvm/bin/clang)
find_program(HOMEBREW_CLANGXX /opt/homebrew/opt/llvm/bin/clang++)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/toolchain.cmake)

set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

cmake_minimum_required(VERSION 3.20)
project(CroCOS LANGUAGES C CXX ASM)

find_package(Python3 3.11 REQUIRED)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_BUILD -O0 -fno-omit-frame-pointer -Wno-error=inline -Wno-comma-subscript")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG_BUILD -O0 -fno-omit-frame-pointer -Wno-error=inline -Wno-comma-subscript")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 --param inline-unit-growth=500 -Wno-error=inline")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 --param inline-unit-growth=500 -Wno-error=inline")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
endif()

add_subdirectory(libraries)
add_subdirectory(tools)
add_subdirectory(kernel)
add_custom_target(build_stress_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/stress_tests
        COMMAND cmake -B build -DCMAKE_BUILD_TYPE=Release
        COMMAND cmake --build build
        COMMENT "Building CroCOS stress tests"
        USES_TERMINAL
)

# The channel through which the kernel reports a FAILURE status to the host.
#
# Without it every termination site wrote the same ACPI poweroff and QEMU exited
# 0 through all of them, so a clean `Goodbye :)`, a debug PANIC and a page fault
# on every CPU were indistinguishable to any automated caller (see
# specs/rcu-phase-4.md, P4-ITEM-006). The success path still uses ACPI and still
# exits 0; kernel::exitToHost writes a failure status here instead, and QEMU
# exits (status << 1) | 1 — always odd, hence never confusable with success.
#
# Deliberately NOT added to qmon/kdebug: those are gdb stubs, where halting on a
# fault so it can be inspected beats exiting out from under the debugger.
set(CROCOS_QEMU_EXIT_DEVICE -device isa-debug-exit,iobase=0xf4,iosize=0x04)

add_custom_target(run
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS Kernel
        COMMAND time qemu-system-x86_64 -kernel kernel/Kernel -no-reboot -nographic -smp 8 -m 256M ${CROCOS_QEMU_EXIT_DEVICE} -d int,cpu_reset,guest_errors -D guestErrors.log -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU"
        USES_TERMINAL
)

add_custom_target(qmon
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS Kernel
        COMMAND time qemu-system-x86_64 -kernel kernel/Kernel -no-reboot -nographic -s -S -smp 16 -m 256M -d int,cpu_reset,guest_errors -D guestErrors.log -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU"
        USES_TERMINAL
)

add_custom_target(run_numa
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS Kernel
        # 3-node NUMA topology:
        #   node 0 — CPUs 0-3, 128M (primary socket)
        #   node 1 — CPUs 4-7,  64M (secondary socket)
        #   node 2 — no CPUs,   64M (memory-only / CXL-style domain)
        # SLIT distances: local=10, 0<->1=21, 0<->2=30, 1<->2=25
        COMMAND time qemu-system-x86_64 -kernel kernel/Kernel -no-reboot -nographic
            -smp 8,sockets=2,cores=4
            -m 256M
            -object memory-backend-ram,id=m0,size=128M
            -object memory-backend-ram,id=m1,size=64M
            -object memory-backend-ram,id=m2,size=64M
            -numa node,nodeid=0,cpus=0-3,memdev=m0
            -numa node,nodeid=1,cpus=4-7,memdev=m1
            -numa node,nodeid=2,memdev=m2
            -numa dist,src=0,dst=1,val=21
            -numa dist,src=1,dst=0,val=21
            -numa dist,src=0,dst=2,val=30
            -numa dist,src=2,dst=0,val=30
            -numa dist,src=1,dst=2,val=25
            -numa dist,src=2,dst=1,val=25
            ${CROCOS_QEMU_EXIT_DEVICE}
            -d int,cpu_reset,guest_errors -D guestErrors.log -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU with 3-node NUMA topology"
        USES_TERMINAL
)

add_custom_target(run_numa_hmat
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS Kernel
        # Same 3-node topology as run_numa but with HMAT latency+bandwidth tables.
        # Latency in picoseconds, bandwidth in KB/s (must be multiples of 1048576 = 1 GiB/s).
        #   node 0 (initiator+target): local 5ns / ~40 GiB/s
        #   node 1 (initiator+target): local 5ns / ~40 GiB/s
        #   node 2 (target only):      CXL-style memory-only domain
        #   0->1 / 1->0: 40ns / ~10 GiB/s
        #   0->2: 80ns / ~8 GiB/s    1->2: 60ns / ~12 GiB/s
        COMMAND time qemu-system-x86_64 -kernel kernel/Kernel -no-reboot -nographic
            -machine q35,hmat=on
            -smp 8,sockets=2,cores=4
            -m 256M
            -object memory-backend-ram,id=m0,size=128M
            -object memory-backend-ram,id=m1,size=64M
            -object memory-backend-ram,id=m2,size=64M
            -numa node,nodeid=0,cpus=0-3,memdev=m0
            -numa node,nodeid=1,cpus=4-7,memdev=m1
            -numa node,nodeid=2,memdev=m2
            -numa hmat-lb,initiator=0,target=0,hierarchy=memory,data-type=access-latency,latency=5000
            -numa hmat-lb,initiator=0,target=0,hierarchy=memory,data-type=access-bandwidth,bandwidth=41943040
            -numa hmat-lb,initiator=0,target=1,hierarchy=memory,data-type=access-latency,latency=40000
            -numa hmat-lb,initiator=0,target=1,hierarchy=memory,data-type=access-bandwidth,bandwidth=10485760
            -numa hmat-lb,initiator=0,target=2,hierarchy=memory,data-type=access-latency,latency=80000
            -numa hmat-lb,initiator=0,target=2,hierarchy=memory,data-type=access-bandwidth,bandwidth=8388608
            -numa hmat-lb,initiator=1,target=0,hierarchy=memory,data-type=access-latency,latency=40000
            -numa hmat-lb,initiator=1,target=0,hierarchy=memory,data-type=access-bandwidth,bandwidth=10485760
            -numa hmat-lb,initiator=1,target=1,hierarchy=memory,data-type=access-latency,latency=5000
            -numa hmat-lb,initiator=1,target=1,hierarchy=memory,data-type=access-bandwidth,bandwidth=41943040
            -numa hmat-lb,initiator=1,target=2,hierarchy=memory,data-type=access-latency,latency=60000
            -numa hmat-lb,initiator=1,target=2,hierarchy=memory,data-type=access-bandwidth,bandwidth=12582912
            ${CROCOS_QEMU_EXIT_DEVICE}
            -d int,cpu_reset,guest_errors -D guestErrors.log -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU with 3-node NUMA topology and HMAT"
        USES_TERMINAL
)

add_custom_target(kdebug
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS Kernel
        COMMAND qemu-system-x86_64 -kernel kernel/Kernel -s -S -no-reboot -nographic -smp 8 -m 256M -d guest_errors -cpu qemu64,+fsgsbase
        COMMENT "Running QEMU"
        USES_TERMINAL
)

# Add unit test target that builds and runs all tests
add_custom_target(unit_tests
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
        COMMAND cmake -B build -DCMAKE_BUILD_TYPE=Debug
        COMMAND cmake --build build --target run_all_tests
        COMMENT "Building and running all CroCOS unit tests"
        USES_TERMINAL
        )
#add_executable(CroCOS)
#target_link_libraries(CroCOS PRIVATE Kernel)