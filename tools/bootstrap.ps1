# The build tools a machine may lack, installed into the repository (no admin rights, nothing system-wide):
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bootstrap.ps1 python   Python into tools\python
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bootstrap.ps1 dotnet   the .NET 10 SDK into tools\dotnet
#
# Python is the python.org build NuGet carries (package "python": python.exe, the standard library and pip, 15 MB),
# pinned and checked against its SHA-256: a newer Python could change what the tools do. To move to another one,
# change PY_VERSION and PY_SHA256 together. The .NET SDK comes from Microsoft's dotnet-install.ps1 (the latest 10.0
# release: about 220 MB to download, 800 MB unpacked). PowerShell, not Python: it runs before there is a Python.
#
# The launcher (and launcher\build.bat) put tools\python and tools\dotnet first on PATH when they are there, so they
# are only installed when the machine has no Python 3.13+ or .NET 10 SDK of its own.
param([Parameter(Mandatory)][ValidateSet('python', 'dotnet')][string]$What)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest's progress bar slows the download down many times over
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$PY_VERSION = '3.13.17'
$PY_SHA256 = 'dce409744b088f94c6e37ba41f8100c384c27e029e843237364c602d30d475e5'
$tools = $PSScriptRoot
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kart-bootstrap-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    if ($What -eq 'python') {
        $dest = Join-Path $tools 'python'
        $url = "https://api.nuget.org/v3-flatcontainer/python/$PY_VERSION/python.$PY_VERSION.nupkg"
        $pkg = Join-Path $tmp 'python.zip'
        Write-Output "downloading Python $PY_VERSION ($url)"
        Invoke-WebRequest -UseBasicParsing $url -OutFile $pkg
        $hash = (Get-FileHash $pkg -Algorithm SHA256).Hash.ToLower()
        if ($hash -ne $PY_SHA256) { throw "SHA-256 mismatch: got $hash, expected $PY_SHA256" }
        Write-Output "unpacking into $dest"
        Expand-Archive $pkg (Join-Path $tmp 'x')
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
        Move-Item (Join-Path $tmp 'x\tools') $dest
        $v = & (Join-Path $dest 'python.exe') -c "import sys, pip; print(sys.version.split()[0], 'with pip', pip.__version__)"
        if ($LASTEXITCODE) { throw "the unpacked python.exe does not run" }
        Write-Output "Python $v in $dest"
    } else {
        $dest = Join-Path $tools 'dotnet'
        $script = Join-Path $tmp 'dotnet-install.ps1'
        Write-Output 'downloading dotnet-install.ps1 (https://dot.net/v1/dotnet-install.ps1)'
        Invoke-WebRequest -UseBasicParsing 'https://dot.net/v1/dotnet-install.ps1' -OutFile $script
        Write-Output "installing the .NET 10 SDK into $dest"
        & $script -Channel 10.0 -InstallDir $dest -NoPath
        $sdks = @(& (Join-Path $dest 'dotnet.exe') --list-sdks)
        if ($LASTEXITCODE -or -not ($sdks -match '^10\.')) { throw "no .NET 10 SDK in $dest after the install" }
        Write-Output ".NET SDK $(($sdks -match '^10\.')[0]) in $dest"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
