add_subdirectory( lemon )
add_subdirectory( re2c )
if( WIN32 )
	add_subdirectory( fixrtext )
endif( WIN32 )
add_subdirectory( updaterevision )
add_subdirectory( zipdir )

if( BUILD_TESTING )
	add_executable( openal_pcm_tests
		openal_pcm_tests.cpp
		${CMAKE_SOURCE_DIR}/src/sound/oaldata.cpp )
	target_include_directories( openal_pcm_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound )
	add_test( NAME openal_pcm COMMAND openal_pcm_tests )

	if( NOT NO_SOUND )
		find_package( OpenAL REQUIRED )
		add_executable( openal_lifecycle_tests
			openal_renderer_lifecycle_tests.cpp
			${CMAKE_SOURCE_DIR}/src/sound/oaldata.cpp
			${CMAKE_SOURCE_DIR}/src/sound/oalsound.cpp )
		target_compile_definitions( openal_lifecycle_tests PRIVATE OAL_LIFECYCLE_TEST )
		target_include_directories( openal_lifecycle_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/sound ${OPENAL_INCLUDE_DIR} )
		target_link_libraries( openal_lifecycle_tests ${OPENAL_LIBRARY} )
		add_test( NAME openal_lifecycle COMMAND openal_lifecycle_tests )
		set_tests_properties( openal_lifecycle PROPERTIES SKIP_RETURN_CODE 77 )
	endif( NOT NO_SOUND )
endif( BUILD_TESTING )

set( CROSS_EXPORTS ${CROSS_EXPORTS} PARENT_SCOPE )
