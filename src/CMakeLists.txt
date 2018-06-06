cmake_minimum_required(VERSION 3.0)


# Create a library called "libwrclient".
project (LIBWRCLIENT VERSION 0.1 LANGUAGES C)

include(FindPkgConfig)

find_package(CURL REQUIRED)
include_directories(${LIBCURL_INCLUDE_DIR})

pkg_check_modules(libmpg123 REQUIRED libmpg123)
include_directories(${libmpg123_INCLUDEDIR})

pkg_check_modules(ogg REQUIRED ogg)
include_directories(${ogg_INCLUDE_DIR})

pkg_check_modules(vorbis REQUIRED vorbis)
include_directories(${vorbis_INCLUDE_DIR})

#add a compile target for our shared library
add_library (wrclient STATIC decode_html_ents.c main.c  mp3.c  ogg.c)

# Add the current directory to include directories
target_include_directories (wrclient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Add the include directory to include directories
target_include_directories (wrclient PUBLIC 
	$<BUILD_INTERFACE:${LIBWRCLIENT_SOURCE_DIR}/include/> 
	$<INSTALL_INTERFACE:include/>
)

#install library and cmake exports for others to find them
INSTALL(
	TARGETS wrclient
	EXPORT libwrclientConfig #export for others to include
	ARCHIVE DESTINATION lib #static if any goes to
	LIBRARY DESTINATION lib #shared if any goes to
	RUNTIME DESTINATION bin #windows location for binary
	INCLUDES DESTINATION include
)

#install header files as well
INSTALL (FILES ${LIBWRCLIENT_SOURCE_DIR}/webradioclient.h DESTINATION include)

# This makes the project importable from the install directory
# Put config file in per-project dir (name MUST match), can also
# just go into <prefix>/cmake.
install(EXPORT libwrclientConfig DESTINATION share/libwrclient/cmake)

# This makes the project importable from the build directory
export(TARGETS wrclient FILE libwrclientConfig.cmake)


find_package(SDL2)

if(${SDL2_FOUND})
	include_directories(${SDL2_INCLUDE_DIRS})
	add_executable (client sdl2client.c)
	target_link_libraries(client wrclient ${SDL2_LIBRARIES}
		${ogg_LIBRARIES} ${vorbis_LIBRARIES} ${libmpg123_LIBRARIES}
		${CURL_LIBRARY})
endif()
