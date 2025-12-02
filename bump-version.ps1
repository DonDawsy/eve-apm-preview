#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Bump project version number
.DESCRIPTION
    Updates VERSION file and regenerates CMake to propagate changes
.PARAMETER Type
    Type of version bump: major, minor, patch
.PARAMETER Version
    Explicit version string (e.g., "1.2.3")
.EXAMPLE
    .\bump-version.ps1 -Type patch
    .\bump-version.ps1 -Version "2.0.0"
#>

param(
    [Parameter(ParameterSetName='Bump')]
    [ValidateSet('major', 'minor', 'patch')]
    [string]$Type,
    
    [Parameter(ParameterSetName='Set')]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$versionFile = Join-Path $PSScriptRoot "VERSION"

if (-not (Test-Path $versionFile)) {
    Write-Error "VERSION file not found at $versionFile"
    exit 1
}

$currentVersion = Get-Content $versionFile -Raw | ForEach-Object { $_.Trim() }

if ($Version) {
    # Explicit version set
    $newVersion = $Version
} elseif ($Type) {
    # Parse current version
    if ($currentVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        Write-Error "Invalid version format in VERSION file: $currentVersion"
        exit 1
    }
    
    $major = [int]$matches[1]
    $minor = [int]$matches[2]
    $patch = [int]$matches[3]
    
    # Bump version
    switch ($Type) {
        'major' {
            $major++
            $minor = 0
            $patch = 0
        }
        'minor' {
            $minor++
            $patch = 0
        }
        'patch' {
            $patch++
        }
    }
    
    $newVersion = "$major.$minor.$patch"
} else {
    Write-Host "Current version: $currentVersion"
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "  .\bump-version.ps1 -Type <major|minor|patch>"
    Write-Host "  .\bump-version.ps1 -Version <x.y.z>"
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\bump-version.ps1 -Type patch     # $currentVersion -> $(if ($currentVersion -match '^(\d+)\.(\d+)\.(\d+)$') { "$($matches[1]).$($matches[2]).$([int]$matches[3]+1)" })"
    Write-Host "  .\bump-version.ps1 -Version 2.0.0  # Set explicit version"
    exit 0
}

Write-Host "Bumping version: $currentVersion -> $newVersion"

# Update VERSION file
Set-Content -Path $versionFile -Value $newVersion -NoNewline

Write-Host "✓ Updated VERSION file"

# Reconfigure CMake to pick up new version
if (Test-Path "build") {
    Write-Host "Reconfiguring CMake..."
    Push-Location
    try {
        Set-Location "build"
        cmake .. | Out-Null
        Write-Host "✓ CMake reconfigured"
    } finally {
        Pop-Location
    }
} else {
    Write-Warning "Build directory not found. Run cmake to configure the project."
}

Write-Host ""
Write-Host "Version bumped to: $newVersion" -ForegroundColor Green
Write-Host "Don't forget to rebuild the project!"
