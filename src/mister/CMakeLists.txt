set(LIB mister)

# The MiSTer hybrid core's own code (MISTER_HYBRID): its header has nothing
# of the engine's, so any library may use it
add_library(${LIB} STATIC
    src/mister_port.c
    include/mister_port.h
)

target_include_directories(${LIB} PUBLIC "./include")
# rt: timer_create() of the profiler (glibc 2.31)
target_link_libraries(${LIB} PRIVATE ${SDL2_LIBRARIES} rt)
