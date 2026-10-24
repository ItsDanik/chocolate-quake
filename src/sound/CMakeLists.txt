set(LIB sound)

set(SOURCES
    src/bgmusic.c
    src/snd_codec.c
    src/snd_codec.h
    src/snd_dma.c
    src/snd_flac.h
    src/snd_mem.c
    src/snd_mix.c
    src/snd_mp3.h
    src/snd_sdl.c
    src/snd_vorbis.h
    src/snd_wave.c
    src/snd_wave.h
)
if(MISTER_HYBRID)
    # the decoders of SDL2_mixer, in one file
    list(APPEND SOURCES src/snd_mister.c src/snd_minimp3.c)
else()
    list(APPEND SOURCES src/snd_flac.c src/snd_mp3.c src/snd_mp3tag.c src/snd_vorbis.c)
endif()
add_library(${LIB} STATIC ${SOURCES})

set(LIBS ${SDL2_LIBRARIES})
if(MISTER_HYBRID)
    target_include_directories(${LIB} PRIVATE ${MISTER_CODECS})
elseif(UNIX AND NOT APPLE)
    list(APPEND LIBS
        ${VORBIS_LIBRARY}
        ${VORBISFILE_LIBRARY}
        ${FLAC_LIBRARY}
        ${MAD_LIBRARY}
    )
else()
    list(APPEND LIBS
        Vorbis::vorbisfile
        mad::mad
        FLAC::FLAC
    )
endif()

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common console mathlib memory)
target_link_libraries(${LIB} PRIVATE ${LIBS} client cmd host model sys)
