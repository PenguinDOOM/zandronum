add_subdirectory( lemon )
add_subdirectory( re2c )
if( WIN32 )
	add_subdirectory( fixrtext )
endif( WIN32 )
add_subdirectory( updaterevision )
add_subdirectory( zipdir )

if( BUILD_TESTING )
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

	if( NOT NO_SOUND AND NOT SERVERONLY )
		find_package( OpenAL REQUIRED )
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
		target_include_directories( openal_lifecycle_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound ${OPENAL_INCLUDE_DIR} )
		target_link_libraries( openal_lifecycle_tests ${OPENAL_LIBRARY} )
		add_test( NAME openal_lifecycle COMMAND openal_lifecycle_tests )
		set_tests_properties( openal_lifecycle PROPERTIES SKIP_RETURN_CODE 77 )
	endif( NOT NO_SOUND AND NOT SERVERONLY )
endif( BUILD_TESTING )

set( CROSS_EXPORTS ${CROSS_EXPORTS} PARENT_SCOPE )
