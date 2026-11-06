@echo off
rem Builds bench_storage.exe (Release-equivalent: /O2 /GL /LTCG, NDEBUG, ASTRA_BUILD_DIST) in place.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /std:c++20 /O2 /GL /DNDEBUG /D__SSE2__ /D__SSE4_2__ /arch:AVX /fp:fast /Zc:__cplusplus /EHsc /nologo /DASTRA_BUILD_DIST /I..\include /I..\vendor\Mosaic\include /I..\tests bench_storage.cpp /Fe:bench_storage.exe /link /LTCG advapi32.lib
echo BUILD_EXIT=%ERRORLEVEL%
