set(TARGET_NAME fssimplewindow)
set(SUB_FOLDER lib)

# Linking framework based on http://www.cmake.org/Wiki/CMake:HowToUseExistingOSXFrameworks
if(APPLE)
	find_library(COCOA_LIB Cocoa)
	find_library(OPENGL_LIB OpenGL)
	mark_as_advanced(COCOA_LIB OPENGL_LIB)
	set(LIB_DEPENDENCY ${COCOA_LIB} ${OPENGL_LIB})
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
	set(LIB_DEPENDENCY EGL GLESv2)
elseif(UNIX)
	option(YSFLIGHT_USE_SDL "Use SDL2 window backend instead of X11/GLX" OFF)
	if(YSFLIGHT_USE_SDL)
		find_package(SDL2 REQUIRED)
		# yssystemfont still needs X11 fonts on Linux.
		set(LIB_DEPENDENCY GL GLU X11 SDL2 ysbitmapfont)
	else()
		set(LIB_DEPENDENCY GL GLU X11)
	endif()
else()
	set(LIB_DEPENDENCY)
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "WindowsStore")
	set(platform_SRCS
		uwp/fsuwpwrapper.cpp
	)
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Android")
	set(platform_SRCS
		android/fsandroidwrapper.cpp
	)
elseif(MSVC)
	set(platform_SRCS
		windows/fswin32keymap.cpp
		windows/fswin32winmain.cpp
		windows/fswin32wrapper.cpp
	)
elseif(APPLE)
	set(platform_SRCS
		macosx/fsmacosxwrappercpp.cpp
		macosx/fsmacosxwrapperobjc.m
	)
elseif(UNIX)
	if(YSFLIGHT_USE_SDL)
		set(platform_SRCS
			sdl/fssdlwrapper.cpp
		)
	else()
		set(platform_SRCS
			glx/fsglxkeymap.cpp
			glx/fsglxwrapper.cpp
		)
	endif()
else()
	set(platform_SRCS "")
endif()

set(SRCS
	${platform_SRCS}
	fssimplewindowcommon.cpp
)

set(HEADERS
fssimplewindow.h
android/fssimplewindow_android.h
)

add_library(${TARGET_NAME} ${SRCS} ${HEADERS})
if(YSFLIGHT_USE_SDL AND TARGET SDL2::SDL2)
	target_link_libraries(${TARGET_NAME} PUBLIC ${LIB_DEPENDENCY} SDL2::SDL2)
else()
	target_link_libraries(${TARGET_NAME} ${LIB_DEPENDENCY})
endif()
target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET ${TARGET_NAME} PROPERTY FOLDER ${SUB_FOLDER})

add_library(fssimplenowindow fssimplewindowcommon.cpp nownd/fssimplenowindow.cpp)
target_include_directories(fssimplenowindow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET fssimplenowindow PROPERTY FOLDER ${SUB_FOLDER})

add_subdirectory(tests)

set(YS_ALL_BATCH_TEST ${YS_ALL_BATCH_TEST} PARENT_SCOPE)
