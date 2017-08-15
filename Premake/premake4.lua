-- helper functions..
function appendToList(l1, l2)
	-- appends elements of l2 to l1
	for i=1, #l2 do
		table.insert(l1, l2[i])
	end
end

function concatLists(...)
	-- returns a list that is a concatenation of all given lists
	-- (ony works for arrays/lists, not generic tables!)
	ret = {}
	for i = 1, select('#', ...) do
		appendToList(ret, select(i, ...))
	end
	
	return ret
end

if os.is("windows") then -- MSVC flags
COMMON_CFLAGS = {
	-- TODO: common MSVC flags
}
else -- GCC/Clang flags for both OSX and Linux
COMMON_CFLAGS = {
	"-pthread",
	"-ffast-math",
	"-fno-strict-aliasing",
	"-ggdb",
	
	-- warnings
	"-Wall",
	"-Wuninitialized",
	--"-Wmaybe-uninitialized",
	
	-- warnings we don't want
--	"-Wno-maybe-uninitialized",
	"-Wno-switch",
	"-Wno-multichar",
	"-Wno-char-subscripts", -- TODO: sometimes run build without this to be sure
	"-Wno-sign-compare",
}
end


solution "webradioclient"
	configurations { "Debug", "Release", "Profile" }
		if os.is("macosx") then
		platforms { "native", "universal" }
		--platforms { "x32", "x64", "universal" }
	else
		platforms {"x32", "x64"}
	end
	
	configuration "Debug"
		flags
		{
			"Symbols",
			"EnableSSE",
			"EnableSSE2",
		}

	configuration "Release"
		flags
		{
			"OptimizeSpeed",
			"EnableSSE",
			"EnableSSE2",
		}

	-- OptimizeSpeed and Symbols do not work together with Visual Studio
	if not os.is("windows") then
		configuration "Profile"
			flags
			{
				"OptimizeSpeed",
				"EnableSSE",
				"EnableSSE2",
				"Symbols",
			}
	end
	
	configuration { "vs*" }
		flags
		{
			"NoManifest",
			"NoMinimalRebuild",
			"No64BitChecks",
			"NoEditAndContinue",
			"NoExceptions"
		}
		buildoptions
		{
			-- multi processor support
			"/MP",
			
			-- warnings to ignore:
			-- "/wd4711", -- smells like old people
			
			-- warnings to force
			
			
		}
		
	
	configuration { "vs*", "Release" }
		buildoptions
		{
			-- turn off Whole Program Optimization
			"/GL-",
			
			-- Inline Function Expansion: Any Suitable (/Ob2)
			--"/Ob2",
			
			-- enable Intrinsic Functions
			"/Oi",
			
			-- Omit Frame Pointers
			"/Oy",
		}
			
	configuration { "vs*", "Profile" }
		buildoptions
		{
			-- Produces a program database (PDB) that contains type information and symbolic debugging information for use with the debugger
			-- /Zi does imply /debug
			"/Zi",
			
			-- turn off Whole Program Optimization
			"/GL-",
			
			-- Inline Function Expansion: Any Suitable (/Ob2)
			--"/Ob2",
			
			-- enable Intrinsic Functions
			"/Oi",
			
			-- Omit Frame Pointers - FIXME: maybe not for profile builds?
			"/Oy",
		}
		linkoptions
		{
			-- turn off Whole Program Optimization
			-- "/LTCG-",
			
			-- create .pdb file
			"/DEBUG",
		}
		
	configuration { "vs*", "x32" }
		defines
		{
			"_CRT_SECURE_NO_DEPRECATE",
			"_CRT_NONSTDC_NO_DEPRECATE",
			--"_CRT_SECURE_NO_WARNINGS",
		}
			
	configuration { "linux" }
		targetprefix ""
		buildoptions (COMMON_CFLAGS)
		linkoptions
		{
			--"-fno-exceptions",
			--"-fno-rtti",
			"-pthread",
			"-ldl",
			"-lm",
		}

	configuration { "macosx" }
		buildoptions( concatLists( COMMON_CFLAGS,
			{ "-x objective-c++",
			  "-fno-exceptions",
			  "-fno-rtti"}) )
		linkoptions
		{
			--"-arch i386",
		
			"-fno-exceptions",
			"-fno-rtti",
		}

project "mylibogg"
	targetname "libogg_static"
	language "C"
	kind "StaticLib"
	flags
	{
		"ExtraWarnings",
	}
	files
	{
		"../libsrc/libogg/src/*.c"
	}
		includedirs
	{
		"../libsrc/libogg/include/",
	}
	
	configuration { "linux" }
		targetprefix "lib"
		buildoptions
		{
			"-fPIC",
		}
		linkoptions
		{
			"-fPIC",
		}

project "mylibvorbis"
	targetname "libvorbis_static"
	language "C"
	kind "StaticLib"
	flags
	{
		"ExtraWarnings",
	}
	files
	{
		"../libsrc/libvorbis/lib/*.c"
	}
	excludes
	{
		"../libsrc/libvorbis/lib/psytune.c",
		"../libsrc/libvorbis/lib/vorbisenc.c",
		-- TODO: if we wanna play vorbis files, vorbisfile.c (=> libvorbisfile)
		--       might be handly
		"../libsrc/libvorbis/lib/vorbisfile.c",
	}
		includedirs
	{
		"../libsrc/libogg/include/",
		"../libsrc/libvorbis/include/",
	}
	
	configuration { "vs*" }
		defines
		{
			-- I need M_PI from math.h
			"_USE_MATH_DEFINES"
		}
	
	configuration { "linux" }
		targetprefix "lib"
		buildoptions
		{
			"-fPIC",
		}
		linkoptions
		{
			"-fPIC",
		}

project "mylibmpg123"
	targetname "libmpg123_static"
	language "C"
	kind "StaticLib"
	flags
	{
		"ExtraWarnings",
	}
	files
	{
		"../libsrc/mpg123/src/libmpg123/compat.c",
		"../libsrc/mpg123/src/libmpg123/dct64.c",
		"../libsrc/mpg123/src/libmpg123/equalizer.c",
		"../libsrc/mpg123/src/libmpg123/feature.c",
		"../libsrc/mpg123/src/libmpg123/format.c",
		"../libsrc/mpg123/src/libmpg123/frame.c",
		"../libsrc/mpg123/src/libmpg123/icy2utf8.c",
		"../libsrc/mpg123/src/libmpg123/icy.c",
		"../libsrc/mpg123/src/libmpg123/id3.c",
		"../libsrc/mpg123/src/libmpg123/index.c",
		"../libsrc/mpg123/src/libmpg123/layer1.c",
		"../libsrc/mpg123/src/libmpg123/layer2.c",
		"../libsrc/mpg123/src/libmpg123/layer3.c",
		"../libsrc/mpg123/src/libmpg123/lfs_alias.c",
		"../libsrc/mpg123/src/libmpg123/libmpg123.c",
		"../libsrc/mpg123/src/libmpg123/ntom.c",
		"../libsrc/mpg123/src/libmpg123/optimize.c",
		"../libsrc/mpg123/src/libmpg123/parse.c",
		"../libsrc/mpg123/src/libmpg123/readers.c",
		"../libsrc/mpg123/src/libmpg123/stringbuf.c",
		"../libsrc/mpg123/src/libmpg123/synth_8bit.c",
		"../libsrc/mpg123/src/libmpg123/synth.c",
		"../libsrc/mpg123/src/libmpg123/synth_real.c",
		"../libsrc/mpg123/src/libmpg123/synth_s32.c",
		"../libsrc/mpg123/src/libmpg123/tabinit.c",
	}
	defines
	{
		-- use the generic non-ASM code
		"OPT_GENERIC"
	}
	
	configuration { "vs*" }
		defines
		{
			"WIN32"
		}
		excludes
		{
			"../libsrc/mpg123/src/libmpg123/lfs_alias.c",
		}
	
	configuration { "linux" }
		targetprefix "lib"
		buildoptions
		{
			"-fPIC",
		}
		linkoptions
		{
			"-fPIC",
		}

project "libwrclient"
	targetname "wrclient"
	language "C"
	kind "SharedLib"
	flags
	{
		"ExtraWarnings"
	}
	files
	{
		"../src/*.c"
	}
	excludes
	{
		"../src/sdl2client.c"
	}
	includedirs
	{
		"../libsrc/libogg/include/",
		"../libsrc/libvorbis/include/",
		-- TODO: this sucks, maybe copy the headers to some include dir
		"../libsrc/mpg123/src/libmpg123/",
	}
	defines
	{
		"WRC__COMPILING_LIB"
	}
	
	links
	{
		"mylibogg",
		"mylibvorbis",
		"mylibmpg123",
	}
	
	targetdir ".."
	
	configuration { "vs*", "x32" }
		includedirs
		{
			"../win32libs/include/" -- for curl
		}
		libdirs
		{
			"../win32libs/lib/" -- for curl
		}
		links
		{
			"libcurl"
		}
	
	configuration { "vs*", "x64" }
		includedirs
		{
			"../win64libs/include/" -- for curl
		}
		libdirs
		{
			"../win64libs/lib/" -- for curl
		}
		links
		{
			"libcurl"
		}
	
	configuration { "linux" }
		targetprefix "lib"
		buildoptions
		{
			"-std=gnu99",
		}
		links
		{
			"curl"
		}

	configuration { "linux", "x64" }
		includedirs
		{
			"../linux64libs/include/" -- for curl
		}
		libdirs
		{
			"../linux64libs/lib/" -- for curl
		}
	configuration { "linux", "x32" }
		-- TODO: those dirs don't actually exist yet
		includedirs
		{
			"../linux32libs/include/" -- for curl
		}
		libdirs
		{
			"../linux32libs/lib/" -- for curl
		}
