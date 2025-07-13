rem call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x86

meson setup --prefix="%~dp0." --buildtype=release --backend vs -D enable_dxgi=false -D enable_d3d8=false -D enable_d3d10=false -D enable_d3d11=false build_release
meson setup --prefix="%~dp0." --buildtype=debug --backend vs -D enable_dxgi=false -D enable_d3d8=false -D enable_d3d10=false -D enable_d3d11=false build_debug
