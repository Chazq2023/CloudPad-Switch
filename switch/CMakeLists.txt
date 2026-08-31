
##################
# borealis dependency (deko3d fork - see switch/borealis vendored from
# xlanor/borealis, the same fork akira uses for its deko3d renderer + FSR)
##################

# switch/borealis/library/CMakeLists.txt is a real, self-contained CMake
# project (unlike the old vendored copy, which we hand-listed source files
# for above). It reads these two variables to select the Switch platform
# block and the deko3d video driver within it, and pulls in yoga/tweeny/fmt/
# tinyxml2 as subdirectories itself - no need for akira's own
# commonOption.cmake/toolchain.cmake bootstrapping, our cmake/switch.cmake
# devkitA64 toolchain already does that job.
set(PLATFORM_SWITCH ON)
set(BOREALIS_USE_DEKO3D ON)
set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/borealis/library)
# library/CMakeLists.txt's set_target_properties(borealis PROPERTIES ...
# UNITY_BUILD ${BRLS_UNITY_BUILD}) errors with "incorrect number of
# arguments" if this is left undefined (empty expansion drops the value for
# the UNITY_BUILD key entirely).
set(BRLS_UNITY_BUILD OFF)
if(CHIAKI_IS_SWITCH)
	set(BRLS_RESOURCES_DIR "romfs:/")
else()
	set(BRLS_RESOURCES_DIR "./switch/res/")
endif()

add_subdirectory(borealis/library borealis)
# The deko3d nanovg backend (lib/extern/nanovg/deko3d/dk_renderer.cpp) uses
# glm, a header-only portlib that library/CMakeLists.txt doesn't add to the
# target's include path itself (akira's own toolchain.cmake apparently adds
# devkitpro portlibs/switch/include globally; we don't use that file - see
# gen_dksh comment above).
target_include_directories(borealis PUBLIC "${DEVKITPRO}/portlibs/switch/include")

# switch/borealis/library/cmake/toolchain.cmake defines gen_dksh() (compiles
# borealis/nanovg's own GLSL shaders to deko3d .dksh via uam) but also
# redefines program_target/ios_bundle/linux_install/add_libromfs, which would
# collide with this file's own targets and cmake/switch.cmake's devkitA64
# setup - so gen_dksh is copied here standalone instead of `include()`-ing
# the whole file.
function(gen_dksh romfs_dir)
	find_program(NX_UAM_EXE NAMES uam HINTS "${DEVKITPRO}/tools/bin")
	if(NOT NX_UAM_EXE)
		message(FATAL_ERROR "Could not find uam: try installing uam")
	endif()
	set(SHADER_BASE "${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders")
	file(MAKE_DIRECTORY "${romfs_dir}")
	execute_process(COMMAND ${NX_UAM_EXE} -s vert -o "${romfs_dir}/fill_vsh.dksh" "${SHADER_BASE}/fill_vsh.glsl" TIMEOUT 5)
	execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_aa_fsh.dksh" "${SHADER_BASE}/fill_aa_fsh.glsl" TIMEOUT 5)
	execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_fsh.dksh" "${SHADER_BASE}/fill_fsh.glsl" TIMEOUT 5)
endfunction()

# TODO(deko3d-fsr-port): output currently goes to the build tree only. Once
# the deko3d video renderer is wired into the app (next stage of this port),
# this needs to land inside whatever directory add_nro_target's romfs= param
# packages (currently switch/res) so the compiled .dksh shaders actually ship
# in the .nro's romfs - res/shaders under source control is the likely home.
gen_dksh("${CMAKE_CURRENT_BINARY_DIR}/dksh_shaders")

# uam must be linked BEFORE EGL because both bundle mesa GLSL symbols; link
# order determines which symbols win with -z,muldefs (see akira's
# cmake/deko3d_shaders.cmake, same author/toolchain as this fork).
#
# TODO(deko3d-fsr-port): the Docker image only ships the `uam` CLI tool
# (used above by gen_dksh to precompile borealis/nanovg's own UI shaders at
# configure time), not libuam itself - akira gets libuam by vendoring
# RSDuck/uam (branch library-target) as its own submodule and building it
# with -Dbuild_as_library=true. Runtime GLSL->dksh compilation (needed for
# the video renderer's FSR shader variants, compiled at runtime rather than
# configure time) requires vendoring that same submodule here; deferred to
# the video-renderer porting stage since this checkpoint doesn't need it.
find_library(UAM_LIB uam HINTS "${DEVKITPRO}/portlibs/switch/lib")
if(NOT UAM_LIB)
	message(STATUS "libuam not found (expected until library/uam is vendored) - runtime shader compilation will be unavailable")
endif()


##################
# chiaki with borealis
##################

find_package(FFMPEG REQUIRED COMPONENTS avcodec avutil swscale)
find_library(SDL2 SDL2)
find_library(SWRESAMPLE swresample)
find_library(DAV1D dav1d)

# find -type f | grep -P '\.(h|cpp)$' | sed 's#\./#\t\t#g'
add_executable(chiaki-borealis WIN32
	src/views/ps_remote_play.cpp
	src/views/cloud_game_list.cpp
	src/settings.cpp
	src/cloudhttp.cpp
	src/cloudauth.cpp
	src/cloudcatalog.cpp
	src/cloudkamaji.cpp
	src/cloudgaikai.cpp
	src/io.cpp
	src/host.cpp
	src/main.cpp
	src/gui.cpp)

set_target_properties(chiaki-borealis PROPERTIES
	CXX_STANDARD 17
	OUTPUT_NAME chiaki)

target_include_directories(chiaki-borealis PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	# TODO(deko3d-fsr-port): switch/include/io.h still does the OpenGL/glad
	# rendering from before this port (video renderer + FSR is the next
	# stage, deliberately kept separate from this UI-API migration) - the
	# new deko3d-driver Borealis no longer exposes glad.h as part of its own
	# public include dirs since it has no OpenGL driver on Switch anymore.
	# Remove this once io.cpp's render path is replaced with deko3d.
	${CMAKE_CURRENT_SOURCE_DIR}/borealis/library/include/borealis/extern/glad)

target_link_libraries(chiaki-borealis
	chiaki-lib
	borealis
	${SDL2}
	dav1d
	FFMPEG::avcodec
	FFMPEG::avutil
	FFMPEG::swscale
	${SWRESAMPLE}
	${SWSCALE})

if(UAM_LIB)
	target_link_libraries(chiaki-borealis ${UAM_LIB})
endif()

# uam and EGL both bundle mesa GLSL symbols; -z,muldefs lets the link
# through and link order above (uam via target_link_libraries before EGL is
# ever pulled in) determines which symbol definitions win. See gen_dksh's
# comment above for why this couldn't just reuse akira's own cmake include.
target_link_options(chiaki-borealis PRIVATE -Wl,-z,muldefs)

if(CHIAKI_IS_SWITCH)
	# libnx is forced by the switch toolchain. The deko3d driver selected
	# above (BOREALIS_USE_DEKO3D) only needs deko3d/nx/m itself (linked
	# transitively via add_subdirectory(borealis/library)) - but the
	# portlib-provided libSDL2.a (io.cpp still calls into SDL2 for audio,
	# input, and windowing) references EGL/glapi/drm_nouveau symbols
	# unconditionally regardless of which video backend actually gets used
	# at runtime, so those still need to be on the link line or the static
	# link fails with "undefined reference to eglGetDisplay" etc. Same
	# requirement as akira's own CMake for PLATFORM_SWITCH, deko3d driver
	# included.
	find_library(Z z)
	find_library(EGL EGL)
	find_library(GLAPI glapi)
	find_library(DRM_NOUVEAU drm_nouveau)
	target_link_libraries(chiaki-borealis ${Z} ${EGL} ${GLAPI} ${DRM_NOUVEAU})
endif()

install(TARGETS chiaki-borealis
	RUNTIME DESTINATION bin
	BUNDLE DESTINATION bin)

if(CHIAKI_IS_SWITCH)
	# Version shown by hbmenu. Release builds can keep this aligned with their
	# release notes by passing -DCLOUDPAD_SWITCH_VERSION=<release version>.
	set(CLOUDPAD_SWITCH_VERSION "alpha 0.0.2" CACHE STRING "CloudPad Switch release version")
	add_nro_target(cloudpad
		chiaki-borealis
		"CloudPad"
		"Chazq"
		"${CLOUDPAD_SWITCH_VERSION}"
		"${CMAKE_CURRENT_SOURCE_DIR}/cloudpad-icon.jpg"
		"${CMAKE_CURRENT_SOURCE_DIR}/res")
endif()
