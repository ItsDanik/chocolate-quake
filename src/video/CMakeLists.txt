set(LIB video)

if(MISTER_HYBRID)
    # the window surface of SDL's MiSTer driver in place of a renderer, the
    # modes from the OSD
    set(SOURCES src/vid_mister.c)
else()
    set(SOURCES
        src/vid_buffers.c
        src/vid_menu.c
        src/vid_modes.c
        src/vid_window.c
    )
endif()
add_library(${LIB} STATIC
    ${SOURCES}
    src/vid_buffers.h
    src/vid_main.c
    src/vid_menu.h
    src/vid_modes.h
    src/vid_window.h
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common)
target_link_libraries(${LIB} PRIVATE
    ${SDL2_LIBRARIES}
    cmd
    console
    host
    input
    renderer
    sound
    sys
    wad
)
if(MISTER_HYBRID)
    target_link_libraries(${LIB} PRIVATE mister)
endif()
