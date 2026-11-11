# Keep the existing Linux IWYU warnings non-fatal.
set(KYTY_IWYU_COMMON "-Xiwyu;--cxx17ns;-Qunused-arguments")
if (LINUX)
	set(KYTY_IWYU_STRICT "")
else()
	set(KYTY_IWYU_STRICT ";-Werror")
endif()

function(include_what_you_use target dirs)
  if (CLANG AND ("${target}" IN_LIST KYTY_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};${KYTY_IWYU_COMMON}${KYTY_IWYU_STRICT}")
    endif()
  endif()
endfunction()

function(include_what_you_use_with_mappings target dirs mappings)
  if (CLANG AND ("${target}" IN_LIST KYTY_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		foreach(map ${mappings})
			list(APPEND mapdirs ";-Xiwyu;--mapping_file=${map}")
		endforeach()
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};${mapdirs};${KYTY_IWYU_COMMON}${KYTY_IWYU_STRICT}")
    endif()
  endif()
endfunction()

function(clang_tidy_check target config headers dirs)
  if (KYTY_ENABLE_CLANG_TIDY AND CLANG AND ("${target}" IN_LIST KYTY_CLANG_TIDY) AND NOT KYTY_CLANG_CL)
    find_program (CLANG_TIDY_EXE NAMES "clang-tidy")
    if (CLANG_TIDY_EXE)
		set(std_arg "-extra-arg=-std=c++${CMAKE_CXX_STANDARD}")
		foreach(dir ${dirs})
			list(APPEND incdirs "-extra-arg=-I${dir}")
		endforeach()
		foreach(header ${headers})
			list(APPEND filter "(${header}.*)")
		endforeach()
		string(REPLACE ";" "|" filter "${filter}")
		if ("${config}" STREQUAL "")
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-warnings-as-errors=*;-header-filter=${filter};${std_arg};${incdirs}")	
		else()		
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-config=${config};-warnings-as-errors=*;-header-filter=${filter};${std_arg};${incdirs}")	
		endif()
    endif()
  endif()
endfunction()

# Optional whole-program builds, off by default (plain builds are unchanged). Clang with lld only.
#   KYTY_LTO=thin         ThinLTO; relinks reuse a ThinLTO cache shared by every build dir.
#   KYTY_PGO=generate     instrumented build for tools/astro-bench/pgo-train.sh (KYTY_PGO_DUMP).
#   KYTY_PGO=use          optimize with KYTY_PGO_PROFILE (an llvm-profdata .profdata file).
# The flags apply to every target, 3rd-party code included. tools/astro-bench/build.sh and
# refbuild.sh set them through KYTY_BUILD_MODE; see "Build modes" in docs/mt-rewrite/PLAN-60fps.md.
macro(config_lto_pgo)

set(KYTY_LTO "OFF" CACHE STRING "Link-time optimization: OFF or thin")
set_property(CACHE KYTY_LTO PROPERTY STRINGS OFF thin)
set(KYTY_PGO "OFF" CACHE STRING "Profile-guided optimization: OFF, generate or use")
set_property(CACHE KYTY_PGO PROPERTY STRINGS OFF generate use)
set(KYTY_PGO_PROFILE "" CACHE FILEPATH "Profile for KYTY_PGO=use (.profdata)")
set(KYTY_LTO_CACHE_DIR "$ENV{HOME}/.cache/kyty-thinlto" CACHE PATH "ThinLTO cache shared by all build dirs")
set(KYTY_LTO_CACHE_POLICY "cache_size_bytes=8g:prune_after=168h" CACHE STRING "lld --thinlto-cache-policy")
set(KYTY_LTO_JOBS "8" CACHE STRING "ThinLTO backend threads at link time (the machine is shared)")

string(TOLOWER "${KYTY_LTO}" kyty_lto)
string(TOLOWER "${KYTY_PGO}" kyty_pgo)
if(kyty_lto MATCHES "^(|off|no|false|0)$")
	set(kyty_lto off)
endif()
if(kyty_pgo MATCHES "^(|off|no|false|0)$")
	set(kyty_pgo off)
endif()

set(kyty_lto_pgo_flags "")
if(NOT kyty_lto STREQUAL "off" OR NOT kyty_pgo STREQUAL "off")
	if(NOT CLANG OR KYTY_CLANG_CL OR NOT KYTY_LINKER STREQUAL "LLD")
		message(FATAL_ERROR "KYTY_LTO and KYTY_PGO need Clang with lld")
	endif()
endif()

if(kyty_lto STREQUAL "thin")
	string(APPEND kyty_lto_pgo_flags " -flto=thin")
	# Every link (tests too) reads bitcode, so every link needs lld.
	add_link_options(-fuse-ld=lld -flto=thin
		"-Wl,--thinlto-cache-dir=${KYTY_LTO_CACHE_DIR}"
		"-Wl,--thinlto-cache-policy=${KYTY_LTO_CACHE_POLICY}"
		"-Wl,--thinlto-jobs=${KYTY_LTO_JOBS}")
	message(STATUS "ThinLTO on, cache ${KYTY_LTO_CACHE_DIR}")
elseif(NOT kyty_lto STREQUAL "off")
	message(FATAL_ERROR "KYTY_LTO must be OFF or thin, not '${KYTY_LTO}'")
endif()

if(kyty_pgo STREQUAL "generate")
	string(APPEND kyty_lto_pgo_flags " -fprofile-generate")
	add_link_options(-fprofile-generate)
	message(STATUS "PGO: instrumented build")
elseif(kyty_pgo STREQUAL "use")
	if(NOT EXISTS "${KYTY_PGO_PROFILE}" OR IS_DIRECTORY "${KYTY_PGO_PROFILE}")
		message(FATAL_ERROR "KYTY_PGO=use needs KYTY_PGO_PROFILE=<file.profdata>; got '${KYTY_PGO_PROFILE}'")
	endif()
	# Compile against a copy named by the profile's content: a new profile changes every compile
	# command, so Ninja rebuilds (it does not track the profile itself), and a moved `current`
	# link or a profile rewritten in place re-runs CMake (configure_file depends on its input).
	file(SHA256 "${KYTY_PGO_PROFILE}" kyty_pgo_hash)
	string(SUBSTRING "${kyty_pgo_hash}" 0 12 KYTY_PGO_PROFILE_HASH)
	set(kyty_pgo_copy "${CMAKE_BINARY_DIR}/pgo-${KYTY_PGO_PROFILE_HASH}.profdata")
	configure_file("${KYTY_PGO_PROFILE}" "${kyty_pgo_copy}" COPYONLY)
	# Code newer than the profile has no counts; that is expected, not a warning.
	string(APPEND kyty_lto_pgo_flags " -fprofile-use=${kyty_pgo_copy}"
		" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin")
	message(STATUS "PGO: profile ${KYTY_PGO_PROFILE} (${KYTY_PGO_PROFILE_HASH})")
elseif(NOT kyty_pgo STREQUAL "off")
	message(FATAL_ERROR "KYTY_PGO must be OFF, generate or use, not '${KYTY_PGO}'")
endif()

string(APPEND KYTY_CPP_FLAGS "${kyty_lto_pgo_flags}")
string(APPEND KYTY_C_FLAGS "${kyty_lto_pgo_flags}")

endmacro()

macro(config_compiler_and_linker)

set(KYTY_WARNINGS_ARE_ERRORS OFF)

set(KYTY_C_FLAGS "")
set(KYTY_CPP_FLAGS "")

# Apple's /usr/bin/ar does not understand @response-file syntax, and macOS has a
# large ARG_MAX so response files aren't needed. Note: the Ninja generator forces a
# response file whenever CMAKE_NINJA_FORCE_RESPONSE_FILE is *defined* (it tests
# definedness, not the value), so on Apple it must be fully unset, not set to 0.
if(APPLE)
	unset(CMAKE_NINJA_FORCE_RESPONSE_FILE CACHE)
else()
	SET(CMAKE_NINJA_FORCE_RESPONSE_FILE 1 CACHE INTERNAL "")
endif()

# Guest code runs natively and needs x86-64-v3 (AVX2, BMI2, FMA), so every host has it.
# No FMA contraction: a * b + c keeps both roundings.
if(KYTY_CLANG_CL)
	if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
		string(REGEX REPLACE "/W[0-4]" "/W3" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	else()
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
	endif()
	
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
			
	set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} /utf-8 /Oy- /clang:-march=x86-64-v3 /clang:-ffp-contract=off /wd4244 /wd4305 /wd4800 /wd4345")
  
	if(KYTY_WARNINGS_ARE_ERRORS)
		#set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} /WX")
	    add_compile_options(/WX)
	endif()

	add_compile_options("$<$<CONFIG:Release>:/O2>")
	add_compile_options("$<$<CONFIG:Release>:/DNDEBUG>")
	add_compile_options("$<$<CONFIG:RelWithDebInfo>:/O2>")
	add_compile_options("$<$<CONFIG:RelWithDebInfo>:/DNDEBUG>")

	set(KYTY_C_FLAGS "${KYTY_CPP_FLAGS}")
	
elseif(CLANG OR GCC)

	if (CLANG)
	    set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -fno-rtti -fno-exceptions -fcolor-diagnostics -finput-charset=UTF-8 -fexec-charset=UTF-8 -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -fmessage-length=0")
	    if (WIN32)
	    	set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -static")
	    endif()
	else()
		set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -fno-exceptions -fdiagnostics-color=always -finput-charset=UTF-8 -fexec-charset=UTF-8 -static-libgcc -static-libstdc++ -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -Wno-unused-value -fmessage-length=0")
	endif()
	set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -march=x86-64-v3 -ffp-contract=off")
	
    if(KYTY_WARNINGS_ARE_ERRORS)
        #set(KYTY_CPP_FLAGS "${KYTY_CPP_FLAGS} -Werror")
	add_compile_options(-Werror)
    endif()	

	add_link_options("-g")
	
	unset(CMAKE_CXX_STANDARD_LIBRARIES CACHE)
	unset(CMAKE_C_STANDARD_LIBRARIES CACHE)
	
	set(KYTY_C_FLAGS "${KYTY_CPP_FLAGS}")

endif()

config_lto_pgo()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${KYTY_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${KYTY_CPP_FLAGS}")

endmacro()
