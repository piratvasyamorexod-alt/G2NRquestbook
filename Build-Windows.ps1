param([string]$Workspace = (Join-Path $PSScriptRoot "_build"))
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$template = Join-Path $Workspace "union-plugin-template"
$templateCommit = "42a8a0cb61a5390696cdbcfe1a918c9e1b25b6f8"
$build = Join-Path $Workspace "compiled"
$release = Join-Path $root "release"
New-Item -ItemType Directory -Force -Path $Workspace, $release | Out-Null

function Check($message) {
    if ($LASTEXITCODE -ne 0) { throw "$message (code $LASTEXITCODE)" }
}

if (!(Test-Path (Join-Path $template ".git"))) {
    git clone --no-checkout --depth 1 https://github.com/Patrix9999/union-plugin-template.git $template
    Check "Failed to fetch Union template"
}
git -C $template cat-file -e "$templateCommit^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) {
    git -C $template fetch --depth 1 origin $templateCommit
    Check "Failed to fetch pinned Union template"
}
git -C $template checkout --detach --force $templateCommit
Check "Failed to select pinned Union template"
git -C $template submodule update --init --recursive
Check "Failed to fetch Union dependencies"

foreach ($source in @("Plugin.ipp", "log_recency.hpp", "log_list.hpp", "mouse_win32.hpp", "quest_cursor.rc", "quest_cursor.cur")) {
    Copy-Item (Join-Path $root "src/$source") (Join-Path $template "src/$source") -Force
}
$textureDir = Join-Path $template "vdf/_Work/Data/Textures/_Compiled"
New-Item -ItemType Directory -Force -Path $textureDir | Out-Null
Copy-Item (Join-Path $root "textures/*-C.TEX") $textureDir -Force
$cmake = Join-Path $template "CMakeLists.txt"
$body = [IO.File]::ReadAllText($cmake)
$body = $body.Replace("project(UnionPlugin", "project(G2NRquestbook")
$body = $body.Replace('VERSION ${PROJECT_VERSION}', 'VERSION 1.1.1.0')
if ($body -notmatch "project\(G2NRquestbook") { throw "Upstream CMake file changed" }
$body += "`r`ntarget_sources(G2NRquestbook PRIVATE src/quest_cursor.rc)`r`n"
[IO.File]::WriteAllText($cmake, $body)

cmake -S $template -B $build -G "Visual Studio 17 2022" -A Win32 -DGOTHIC_API_G2A=ON
Check "CMake configure failed"
cmake --build $build --config Release --parallel 2
Check "C++ or VDF build failed"
Copy-Item (Join-Path $build "vdf/G2NRquestbook.vdf") (Join-Path $release "G2NRquestbook.vdf") -Force
