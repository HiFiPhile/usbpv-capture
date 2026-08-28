param(
    [string]$SourceDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'vendor\original\windows-x64')
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$original = Join-Path $repoRoot 'vendor\original\windows-x64'
$destination = Join-Path $repoRoot 'vendor\windows-x64'

function Copy-WithPatchedAsciiNames {
    param(
        [Parameter(Mandatory)] [string]$Source,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [hashtable]$Replacements
    )

    $bytes = [System.IO.File]::ReadAllBytes($Source)

    foreach ($oldName in $Replacements.Keys) {
        $newName = [string]$Replacements[$oldName]
        $oldBytes = [System.Text.Encoding]::ASCII.GetBytes($oldName)
        $newBytes = [System.Text.Encoding]::ASCII.GetBytes($newName)
        if ($newBytes.Length -gt $oldBytes.Length) {
            throw "Replacement '$newName' is longer than '$oldName'"
        }

        $matches = [System.Collections.Generic.List[int]]::new()
        for ($offset = 0; $offset -le $bytes.Length - $oldBytes.Length; ++$offset) {
            $equal = $true
            for ($index = 0; $index -lt $oldBytes.Length; ++$index) {
                if ($bytes[$offset + $index] -ne $oldBytes[$index]) {
                    $equal = $false
                    break
                }
            }
            if ($equal) {
                $matches.Add($offset)
                $offset += $oldBytes.Length - 1
            }
        }

        if ($matches.Count -ne 1) {
            throw "Expected one '$oldName' string in '$Source'; found $($matches.Count)"
        }

        $position = $matches[0]
        [Array]::Clear($bytes, $position, $oldBytes.Length)
        [Array]::Copy($newBytes, 0, $bytes, $position, $newBytes.Length)
    }

    [System.IO.File]::WriteAllBytes($Destination, $bytes)
}

$required = @(
    'usbpv_lib.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)
foreach ($name in $required) {
    $source = Join-Path $SourceDirectory $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required vendor file not found: $source"
    }
}

New-Item -ItemType Directory -Path $original -Force | Out-Null
New-Item -ItemType Directory -Path $destination -Force | Out-Null
if ([IO.Path]::GetFullPath($SourceDirectory) -ne [IO.Path]::GetFullPath($original)) {
    foreach ($name in $required) {
        Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) `
            -Destination (Join-Path $original $name) -Force
    }
}

Copy-WithPatchedAsciiNames `
    -Source (Join-Path $SourceDirectory 'usbpv_lib.dll') `
    -Destination (Join-Path $destination 'usbpv_lib.dll') `
    -Replacements @{
        'libgcc_s_seh-1.dll' = 'upvgcc.dll'
        'libstdc++-6.dll' = 'upvstd.dll'
        'libwinpthread-1.dll' = 'upvpth.dll'
    }

Copy-WithPatchedAsciiNames `
    -Source (Join-Path $SourceDirectory 'libgcc_s_seh-1.dll') `
    -Destination (Join-Path $destination 'upvgcc.dll') `
    -Replacements @{
        'libgcc_s_seh-1.dll' = 'upvgcc.dll'
        'libwinpthread-1.dll' = 'upvpth.dll'
    }

Copy-WithPatchedAsciiNames `
    -Source (Join-Path $SourceDirectory 'libstdc++-6.dll') `
    -Destination (Join-Path $destination 'upvstd.dll') `
    -Replacements @{
        'libgcc_s_seh-1.dll' = 'upvgcc.dll'
        'libstdc++-6.dll' = 'upvstd.dll'
        'libwinpthread-1.dll' = 'upvpth.dll'
    }

Copy-WithPatchedAsciiNames `
    -Source (Join-Path $SourceDirectory 'libwinpthread-1.dll') `
    -Destination (Join-Path $destination 'upvpth.dll') `
    -Replacements @{
        'libwinpthread-1.dll' = 'upvpth.dll'
    }

Get-Item -LiteralPath `
    (Join-Path $destination 'usbpv_lib.dll'), `
    (Join-Path $destination 'upvgcc.dll'), `
    (Join-Path $destination 'upvstd.dll'), `
    (Join-Path $destination 'upvpth.dll')
