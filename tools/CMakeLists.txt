add_subdirectory( lemon )
add_subdirectory( re2c )
if( WIN32 )
	add_subdirectory( fixrtext )
endif( WIN32 )
add_subdirectory( updaterevision )
add_subdirectory( zipdir )

set( EFFECTIVE_NO_SOUND ${NO_SOUND} )
if( SERVERONLY )
	set( EFFECTIVE_NO_SOUND ON )
endif( SERVERONLY )

if( BUILD_TESTING AND NOT EFFECTIVE_NO_SOUND )
	add_executable( audio_decoder_tests
		audio_decoder_tests.cpp
		${CMAKE_SOURCE_DIR}/src/sound/audio_decoder.cpp
		${CMAKE_SOURCE_DIR}/src/sound/audio_decoder_miniaudio.cpp
		${CMAKE_SOURCE_DIR}/src/sound/audio_decoder_vorbis.cpp
		${CMAKE_SOURCE_DIR}/src/sound/audio_timetag.cpp
		${CMAKE_SOURCE_DIR}/src/sound/thirdparty/stb/stb_vorbis.c )
	target_include_directories( audio_decoder_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound )
	target_compile_definitions( audio_decoder_tests PRIVATE AUDIO_DECODER_TESTING AUDIO_DECODER_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tools/testdata/audio" )
	add_test( NAME audio_decoder COMMAND audio_decoder_tests )

	add_executable( openal_pcm_tests
		openal_pcm_tests.cpp
		${CMAKE_SOURCE_DIR}/src/sound/oaldata.cpp )
	target_include_directories( openal_pcm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound )
	add_test( NAME openal_pcm COMMAND openal_pcm_tests )

	add_executable( midi_device_selection_tests
		midi_device_selection_tests.cpp
		${CMAKE_SOURCE_DIR}/src/sound/music_mididevice_policy.cpp )
	target_include_directories( midi_device_selection_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/sound )
	add_test( NAME midi_device_selection COMMAND midi_device_selection_tests )

	if( NOT EFFECTIVE_NO_SOUND )
		find_package( OpenAL REQUIRED )
		find_path( OPENAL_INCLUDE_ROOT AL/al.h
			PATHS ${OPENAL_INCLUDE_DIR} ${OPENAL_INCLUDE_DIR}/.. )
		add_executable( openal_lifecycle_tests
			openal_renderer_lifecycle_tests.cpp
			${CMAKE_SOURCE_DIR}/src/sound/audio_decoder.cpp
			${CMAKE_SOURCE_DIR}/src/sound/audio_decoder_miniaudio.cpp
			${CMAKE_SOURCE_DIR}/src/sound/audio_decoder_vorbis.cpp
			${CMAKE_SOURCE_DIR}/src/sound/audio_timetag.cpp
			${CMAKE_SOURCE_DIR}/src/sound/oaldata.cpp
			${CMAKE_SOURCE_DIR}/src/sound/oalsound.cpp
			${CMAKE_SOURCE_DIR}/src/sound/thirdparty/stb/stb_vorbis.c )
		target_compile_definitions( openal_lifecycle_tests PRIVATE OAL_LIFECYCLE_TEST AUDIO_DECODER_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tools/testdata/audio" )
		target_include_directories( openal_lifecycle_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound ${OPENAL_INCLUDE_ROOT} )
		target_link_libraries( openal_lifecycle_tests ${OPENAL_LIBRARY} )
		add_test( NAME openal_lifecycle COMMAND openal_lifecycle_tests )
		set_tests_properties( openal_lifecycle PROPERTIES SKIP_RETURN_CODE 77 )
		add_test( NAME openal_phase2_unit COMMAND openal_lifecycle_tests --phase2-unit-only )
	endif( NOT EFFECTIVE_NO_SOUND )
endif( BUILD_TESTING AND NOT EFFECTIVE_NO_SOUND )

set( CROSS_EXPORTS ${CROSS_EXPORTS} PARENT_SCOPE )
