@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
msbuild "poupou p2p stream.sln" /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /v:minimal
