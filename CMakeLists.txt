cmake_minimum_required(VERSION 3.24)

# Working name. Change PROJECT_NAME only here; the DLL, log file and plugin
# declaration all use it.
project(
	SkyrimDiagnostics
	VERSION 0.1.0
	DESCRIPTION "Performance and crash diagnostics for Skyrim SE/AE/VR"
	LANGUAGES CXX
)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset release")
endif()

# Short commit ID of this build. The log, the reports and the overlay show
# it, so it is always clear which build is installed.
execute_process(
	COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	OUTPUT_VARIABLE SKYDIAG_BUILD
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT SKYDIAG_BUILD)
	set(SKYDIAG_BUILD "unknown")
endif()
message(STATUS "SkyrimDiagnostics build: ${SKYDIAG_BUILD}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CommonLibSSE-NG. One DLL for SE, AE and VR. All three runtimes must stay ON,
# or the prebuilt CommonLib bundle is refused and the build compiles CommonLib
# from source (about 16 minutes).
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng commonlib EXCLUDE_FROM_ALL)

option(SKYDIAG_BUILD_TESTS "Build the test programs" ON)

# Crash handler core. Win32 only: no CommonLibSSE, no precompiled header.
add_library(
	skydiag_crash STATIC
	src/crash/CrashHandler.cpp
	src/crash/CrashHandler.h
	src/crash/MemTrack.cpp
	src/crash/MemTrack.h
	src/crash/Profiler.h
	src/crash/Session.cpp
	src/crash/Session.h
	src/crash/Text.h
)
target_link_libraries(skydiag_crash PUBLIC dbghelp.lib psapi.lib)
if(MSVC)
	target_compile_options(skydiag_crash PRIVATE /W4 /permissive- /utf-8 /Zi)
endif()

# Frame statistics: standard C++ only.
# The default configuration file is the file SkyrimDiagnostics.ini at the
# top level. Its bytes go into the program as an array.
set(SKYDIAG_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/SkyrimDiagnostics.ini")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SKYDIAG_CONFIG_FILE}")
file(READ "${SKYDIAG_CONFIG_FILE}" SKYDIAG_CONFIG_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SKYDIAG_CONFIG_BYTES "${SKYDIAG_CONFIG_HEX}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/DefaultConfig.inc"
	"// Made by CMake from SkyrimDiagnostics.ini. Do not change.\nstatic const unsigned char kDefaultConfig[] = { ${SKYDIAG_CONFIG_BYTES} 0x00 };\n")

add_library(
	skydiag_stats STATIC
	src/Config.cpp
	src/Config.h
	src/gfx/Advisor.cpp
	src/gfx/Advisor.h
	src/gfx/FrameStats.cpp
	src/gfx/FrameStats.h
	src/ModMap.cpp
	src/ModMap.h
	src/OverlayLayout.cpp
	src/OverlayLayout.h
	src/Report.cpp
	src/Report.h
	src/Settings.cpp
	src/Settings.h
)

# Present hook and overlay: Direct3D 11 and Dear ImGui, no CommonLibSSE.
find_package(imgui CONFIG REQUIRED)
target_include_directories(skydiag_stats PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_library(skydiag_gfx STATIC src/gfx/Overlay.cpp src/gfx/Overlay.h)
target_link_libraries(skydiag_gfx PUBLIC skydiag_stats skydiag_crash imgui::imgui d3d11.lib dxgi.lib)

if(MSVC)
	target_compile_options(skydiag_stats PRIVATE /W4 /permissive- /utf-8 /Zi)
	target_compile_options(skydiag_gfx PRIVATE /W4 /permissive- /utf-8 /Zi)
endif()

add_commonlibsse_plugin(
	${PROJECT_NAME}
	NAME "${PROJECT_NAME}"
	AUTHOR "YellowCourtesy"
	VERSION "${PROJECT_VERSION}"
	SOURCES
		src/GameDescribers.cpp
		src/GameDescribers.h
		src/Papyrus.cpp
		src/Papyrus.h
		src/main.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE skydiag_crash skydiag_stats skydiag_gfx)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		SKYDIAG_VERSION="${PROJECT_VERSION} build ${SKYDIAG_BUILD}"
)

if(MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/W4
			/permissive-
			/utf-8
			/Zc:preprocessor # required by CommonLibSSE-NG headers
			/Zi              # always make a PDB: the crash reporter will need symbols
	)
	target_link_options(
		${PROJECT_NAME}
		PRIVATE
			/DEBUG:FULL
			/OPT:REF
			/OPT:ICF
	)
endif()

# Install tree = a mod folder that MO2 can install directly.
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "SKSE/Plugins" COMPONENT main)
install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION "SKSE/Plugins" COMPONENT main OPTIONAL)
install(FILES SkyrimDiagnostics.ini DESTINATION "SKSE/Plugins" COMPONENT main)
install(FILES mo2/skyrim_diagnostics_export.py DESTINATION "MO2-plugin" COMPONENT main)

if(SKYDIAG_BUILD_TESTS)
	# badmod.dll plays the role of a defective mod, crash_test.exe the game.
	add_library(badmod SHARED tests/badmod.cpp)
	# A separate SKSE plugin that makes faults on purpose in the real game.
	add_commonlibsse_plugin(
		SkyrimDiagnosticsFaultTest
		NAME "SkyrimDiagnosticsFaultTest"
		AUTHOR "YellowCourtesy"
		VERSION "${PROJECT_VERSION}"
		SOURCES tests/faulttest/main.cpp
	)
	target_precompile_headers(SkyrimDiagnosticsFaultTest PRIVATE src/PCH.h)
	if(MSVC)
		target_compile_options(SkyrimDiagnosticsFaultTest PRIVATE /permissive- /utf-8 /Zc:preprocessor /Zi)
		target_link_options(SkyrimDiagnosticsFaultTest PRIVATE /DEBUG:FULL)
	endif()
	install(TARGETS SkyrimDiagnosticsFaultTest RUNTIME DESTINATION "SKSE/Plugins" COMPONENT faulttest)

	add_executable(crash_test tests/crash_test.cpp)
	target_link_libraries(crash_test PRIVATE skydiag_crash skydiag_stats badmod)
	if(MSVC)
		target_compile_options(badmod PRIVATE /Zi)
		target_link_options(badmod PRIVATE /DEBUG:FULL)
		target_link_options(crash_test PRIVATE /DEBUG:FULL)
	endif()
endif()
