# One-step release: build LuigisMansionRecomp, package it as
# out/LuigisMansionRecompiled-<version>-windows-x64.zip, build the Linux packages in WSL
# (tools/release_linux.sh; skipped with -SkipLinux or when WSL is not installed), fetch the macOS
# package that GitHub Actions built from the same commit (docs/MACOS.md "Releases"; skipped with
# -SkipMac), then publish GitHub release v<version> with every package and
# packaging/release_notes/v<version>.md (download table filled in).
# Publishes with the GitHub CLI (gh) when it is installed and logged in; otherwise opens
# the "new release" page in the browser, filled in, and the zip's folder in Explorer.
#   powershell -ExecutionPolicy Bypass -File tools\release_windows.ps1 [-Preset windows-clang] [-SkipLinux] [-SkipMac]
# -MacCheckOnly: only find/wait for/download/verify the macOS CI package of HEAD (into out/);
# nothing is built or published (a dry run of the macOS step, e.g. for a branch).
# (tools\release_windows.bat does the same on a double-click.)
param(
    [string]$Preset = "windows-clang",
    [switch]$SkipLinux,
    [switch]$SkipMac,
    [switch]$MacCheckOnly
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$repo = "rob646464/luigis-mansion-recompiled"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }

# Runs gh (or git) and returns its stdout; $LASTEXITCODE is the tool's. Both print status to
# stderr, which Windows PowerShell turns into error records (fatal under
# ErrorActionPreference=Stop): only the exit code matters, so stderr goes to the console as text.
function Tool {
    $exe = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $exe @rest 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host $_.ToString() } else { $_ }
        }
    } finally {
        $ErrorActionPreference = $prev
    }
}

# The macOS package is built and packaged by GitHub Actions (.github/workflows/build.yml, job
# "macOS arm64") from the release commit: nobody here builds or runs macOS. Find (or start) the
# workflow run for $sha, wait for it, download the package artifact and check it. Returns the
# package path, or $null when it could not be fetched ($why says why).
function Get-CiMacPackage([string]$sha, [string]$branch, [string]$version, [string]$outDir) {
    $pkgName = "LuigisMansionRecompiled-$version-macos-arm64.tar.gz"
    $script:why = ""
    $runs = Tool gh run list --repo $repo --workflow build.yml --commit $sha --limit 20 --json "databaseId,event,status,conclusion,createdAt"
    $run = $null
    if (-not $LASTEXITCODE -and $runs) {
        # (Windows PowerShell's ConvertFrom-Json passes a JSON array on as one object: no @() here.)
        $list = ($runs -join "`n") | ConvertFrom-Json
        # Newest first; a cancelled run (superseded by a newer push) is no use.
        $run = $list | Where-Object { $_.conclusion -ne "cancelled" } | Sort-Object createdAt -Descending | Select-Object -First 1
    }
    if (-not $run) {
        Write-Host "No build.yml run for $sha yet: starting one on '$branch' (workflow_dispatch)."
        Tool gh workflow run build.yml --repo $repo --ref $branch | Out-Host
        if ($LASTEXITCODE) { $script:why = "could not start the GitHub Actions build"; return $null }
        for ($i = 0; $i -lt 30 -and -not $run; $i++) {
            Start-Sleep -Seconds 5
            $runs = Tool gh run list --repo $repo --workflow build.yml --commit $sha --limit 5 --json "databaseId,event,status,conclusion,createdAt"
            if (-not $LASTEXITCODE -and $runs) {
                $list = ($runs -join "`n") | ConvertFrom-Json
                $run = $list | Sort-Object createdAt -Descending | Select-Object -First 1
            }
        }
        if (-not $run) { $script:why = "the started GitHub Actions run did not show up"; return $null }
    }
    $id = $run.databaseId
    Write-Host "GitHub Actions run: https://github.com/$repo/actions/runs/$id ($($run.event), $($run.status))"
    # Like `gh run watch`, one line per minute instead of a redrawn table (the output is not a
    # terminal here). Ctrl+C stops the release.
    $info = $null
    $last = ""
    for (;;) {
        $view = Tool gh run view $id --repo $repo --json "status,conclusion,headSha,jobs"
        if ($LASTEXITCODE) { $script:why = "could not read run $id"; return $null }
        $info = ($view -join "`n") | ConvertFrom-Json
        $line = ($info.jobs | ForEach-Object { "$($_.name): $(if ($_.conclusion) { $_.conclusion } else { $_.status })" }) -join ", "
        if ($line -ne $last) { Write-Host "  $(Get-Date -Format HH:mm) $line"; $last = $line }
        if ($info.status -eq "completed") { break }
        Start-Sleep -Seconds 60
    }
    if ($info.headSha -ne $sha) { $script:why = "run $id built $($info.headSha), not $sha"; return $null }
    $macJob = $info.jobs | Where-Object { $_.name -eq "macOS arm64" } | Select-Object -First 1
    if (-not $macJob -or $macJob.conclusion -ne "success") {
        $c = if ($macJob) { $macJob.conclusion } else { "missing" }
        $script:why = "the macOS job of run $id ended with '$c' (run: $($info.conclusion))"
        return $null
    }
    if ($info.conclusion -ne "success") {
        Write-Host "Note: run $id ended with '$($info.conclusion)', but its macOS job succeeded." -ForegroundColor Yellow
    }

    $dlDir = Join-Path $outDir "ci-macos-$id"
    if (Test-Path $dlDir) { Remove-Item -Recurse -Force $dlDir }
    New-Item -ItemType Directory -Force $dlDir | Out-Null
    Tool gh run download $id --repo $repo --name $pkgName --dir $dlDir | Out-Host
    if ($LASTEXITCODE) { $script:why = "could not download artifact $pkgName of run $id"; return $null }
    $files = @(Get-ChildItem -File -Recurse $dlDir)
    if ($files.Count -ne 1 -or $files[0].Name -ne $pkgName) {
        $script:why = "artifact $pkgName holds '$($files.Name -join ', ')' instead of one $pkgName"
        return $null
    }
    $pkg = $files[0].FullName
    $size = $files[0].Length
    # The package carries clang + ld64.lld from LLVM: well over 20 MB; more than 1 GB is wrong too.
    if ($size -lt 20MB -or $size -gt 1GB) { $script:why = "$pkgName is $([math]::Round($size / 1MB)) MB"; return $null }
    # Windows' tar.exe (bsdtar) lists .tar.gz. The updater needs the same files as on Linux
    # (src/port/update.cpp kExpected), under one top folder.
    $listing = Tool tar.exe -tzf $pkg
    if ($LASTEXITCODE) { $script:why = "$pkgName is not a readable .tar.gz"; return $null }
    $top = "LuigisMansionRecompiled-macos-arm64"
    foreach ($f in @("LuigisMansionRecomp", "pipelines.bin", "llvm/bin/clang", "llvm/bin/ld64.lld", "README.txt", "NOTICE.md")) {
        if (-not ($listing -contains "$top/$f")) { $script:why = "$pkgName has no $top/$f"; return $null }
    }
    $outside = @($listing | Where-Object { $_ -and -not $_.StartsWith("$top/") -and $_ -ne $top })
    if ($outside.Count) { $script:why = "$pkgName has files outside $top/: $($outside[0])"; return $null }
    $dest = Join-Path $outDir $pkgName
    Copy-Item -Force $pkg $dest
    $h = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLower()
    [IO.File]::WriteAllText("$dest.sha256", "$h  $pkgName`n", (New-Object Text.UTF8Encoding $false))
    Write-Host "macOS package OK: $pkgName, $([math]::Round($size / 1MB)) MB, $($listing.Count) entries, sha256 $h (run $id)" -ForegroundColor Green
    return $dest
}

Push-Location $root
try {
    # Version from CMakeLists.txt: project(lmport ... VERSION x.y.z)
    $m = Select-String -Path CMakeLists.txt -Pattern 'project\(lmport .*VERSION ([0-9.]+)\)' | Select-Object -First 1
    if (-not $m) { throw "project version not found in CMakeLists.txt" }
    $version = $m.Matches[0].Groups[1].Value
    $tag = "v$version"
    $notesSrc = "packaging/release_notes/$tag.md"
    if (-not (Test-Path $notesSrc)) { throw "release notes $notesSrc missing" }
    Step "Releasing Luigi's Mansion Recompiled $version ($tag)"

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne "main") { Write-Host "Warning: building branch '$branch', not main." -ForegroundColor Yellow }
    $sha = (git rev-parse HEAD).Trim()

    $gh = Get-Command gh -ErrorAction SilentlyContinue
    $ghOk = $false
    if ($gh) {
        # `gh auth status` fails if any stored account fails (e.g. an old suspended one), even when
        # the active account works: ask the API as the active account instead.
        Tool gh api user --jq .login | Out-Null
        $ghOk = ($LASTEXITCODE -eq 0)
    }

    # The macOS package comes from GitHub Actions' build of this very commit, so the commit has
    # to be on GitHub first (a push to main starts the build; it runs while Windows and Linux
    # build here).
    $wantMac = -not $SkipMac
    if ($wantMac -and -not $ghOk) {
        Write-Host "The GitHub CLI (gh) is not installed or not logged in: the macOS package cannot be fetched." -ForegroundColor Yellow
        $answer = Read-Host "Publish without a macOS package? (y/n)"
        if ($answer -notmatch '^[yY]') { throw "stopped: no gh for the macOS package" }
        $wantMac = $false
    }
    if ($wantMac) {
        Step "Checking that $sha is on GitHub (GitHub Actions builds the macOS package from it)"
        if (git status --porcelain --untracked-files=no --ignore-submodules=dirty) {
            Write-Host "Warning: uncommitted changes; the Windows and Linux packages include them, the macOS package does not." -ForegroundColor Yellow
        }
        Tool git fetch origin $branch | Out-Host
        $remote = (Tool git rev-parse "origin/$branch" | Select-Object -First 1)
        if ($LASTEXITCODE -or "$remote".Trim() -ne $sha) {
            Write-Host "origin/$branch is $remote, this checkout is $sha." -ForegroundColor Yellow
            $answer = Read-Host "Push '$branch' to origin now, so GitHub Actions builds it? (y/n)"
            if ($answer -notmatch '^[yY]') { throw "stopped: $sha is not on GitHub (or rerun with -SkipMac)" }
            Tool git push origin "HEAD:refs/heads/$branch" | Out-Host
            if ($LASTEXITCODE) { throw "git push failed" }
        }
    }

    if ($MacCheckOnly) {
        if (-not $wantMac) { throw "-MacCheckOnly needs the macOS step (gh, no -SkipMac)" }
        Step "Fetching the macOS package from GitHub Actions (check only)"
        $outDir = Join-Path $root "out"
        New-Item -ItemType Directory -Force $outDir | Out-Null
        $macPkg = Get-CiMacPackage $sha $branch $version $outDir
        if (-not $macPkg) { throw "no macOS package: $script:why" }
        return
    }

    Step "Building (this can take a while)"
    & powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\build.ps1" -Preset $Preset -Target LuigisMansionRecomp
    if ($LASTEXITCODE) { throw "build failed" }

    foreach ($p in @("$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")) {
        if (Test-Path $p) { $env:PATH = "$p;$env:PATH" }
    }

    Step "Packaging"
    $name = "LuigisMansionRecompiled-$version-windows-x64"
    $outDir = Join-Path $root "out"
    $stage = Join-Path $outDir $name
    $zip = Join-Path $outDir "$name.zip"
    New-Item -ItemType Directory -Force $outDir | Out-Null
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    if (Test-Path $zip) { Remove-Item -Force $zip }
    cmake --install "build/$Preset" --prefix $stage
    if ($LASTEXITCODE) { throw "cmake --install failed" }
    foreach ($f in @("LuigisMansionRecomp.exe", "pipelines.bin", "llvm\bin\clang.exe", "llvm\bin\lld-link.exe")) {
        if (-not (Test-Path (Join-Path $stage $f))) { throw "package is missing $f" }
    }
    Compress-Archive -Path $stage -DestinationPath $zip
    $assets = @(@{ File = $zip; Platform = "Windows 10/11, 64-bit, Direct3D 12" })

    $linuxNote = ""
    if ($SkipLinux) {
        $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
    } elseif (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        Write-Host "WSL is not installed: no Linux package (install it with 'wsl --install')." -ForegroundColor Yellow
        $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
    } else {
        Step "Building the Linux packages in WSL (asks for your Linux password if packages are missing)"
        & wsl.exe --cd "$root" bash tools/release_linux.sh $version out
        $tgz = Join-Path $outDir "LuigisMansionRecompiled-$version-linux-x86_64.tar.gz"
        if ($LASTEXITCODE -or -not (Test-Path $tgz)) {
            Write-Host ""
            Write-Host "The Linux build failed (messages above)." -ForegroundColor Red
            $answer = Read-Host "Publish without the Linux package? (y/n)"
            if ($answer -notmatch '^[yY]') { throw "stopped: Linux build failed" }
            $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
        } else {
            # Only the .tar.gz is published (it keeps file permissions); the .zip release_linux.sh
            # also writes stays in out/.
            $assets += @{ File = $tgz; Platform = "Linux x86_64, glibc 2.34+, Vulkan" }
        }
    }

    # macOS: unsigned and not notarized; built and packaged by GitHub Actions, never run by us.
    $macNote = ""
    $noMacNote = "macOS: no $version package.`n"
    if (-not $wantMac) {
        $macNote = $noMacNote
    } else {
        Step "Fetching the macOS package from GitHub Actions"
        $macPkg = Get-CiMacPackage $sha $branch $version $outDir
        if (-not $macPkg) {
            Write-Host ""
            Write-Host "No macOS package: $script:why" -ForegroundColor Red
            $answer = Read-Host "Publish without the macOS package? (y/n)"
            if ($answer -notmatch '^[yY]') { throw "stopped: macOS package unavailable ($script:why)" }
            $macNote = $noMacNote
        } else {
            $assets += @{ File = $macPkg; Platform = "macOS 13+, Apple silicon (M1 or newer), Metal; unsigned" }
            $macNote = "macOS: the package is built by GitHub Actions and is not signed or notarized by Apple. " +
                       "Before the first start run ``xattr -dr com.apple.quarantine`` on the unpacked folder, " +
                       "or right-click LuigisMansionRecomp > Open (see README.txt). The macOS version is community-tested only.`n"
        }
    }

    # The in-game updater (docs/SETUP.md "Updates") relies on: tag v<version>, the asset names
    # <prefix>-<version>-windows-x64.zip / -linux-x86_64.tar.gz / -macos-arm64.tar.gz, and this
    # table's "| `<file>` | <platform> | `<sha256>` |" rows (its fallback when GitHub gives no digest).
    $rows = foreach ($a in $assets) {
        $h = (Get-FileHash -Algorithm SHA256 $a.File).Hash.ToLower()
        $leaf = Split-Path -Leaf $a.File
        Write-Host "$leaf  $([math]::Round((Get-Item $a.File).Length / 1MB)) MB  sha256 $h"
        "| ``$leaf`` | $($a.Platform) | ``$h`` |"
    }
    # {{MAC_NOTE}} where the notes have it, else right after the Linux note (older templates).
    $notes = Get-Content -Raw -Encoding UTF8 $notesSrc
    if ($notes.Contains("{{MAC_NOTE}}")) { $notes = $notes.Replace("{{MAC_NOTE}}", $macNote) }
    else { $linuxNote = $linuxNote + $macNote }
    $notes = $notes.Replace("{{DOWNLOAD_ROWS}}", ($rows -join "`n")).Replace("{{LINUX_NOTE}}", $linuxNote)
    # Every release page starts with the Discord link (user request 2026-11-12).
    $discord = "**Discord:** news, help and bug reports for all my projects: https://discord.gg/cM4chZtgh7"
    if (-not $notes.Contains("discord.gg/")) { $notes = $discord + "`n`n" + $notes }
    $notesFile = Join-Path $outDir "$tag-notes.md"
    [IO.File]::WriteAllText($notesFile, $notes, (New-Object Text.UTF8Encoding $false))
    $title = "Luigi's Mansion Recompiled $version"

    Step "Publishing GitHub release $tag"
    # The tag goes on the commit that was built ($sha when it is on GitHub, which the macOS
    # step made sure of; else the branch as before).
    $target = if ($wantMac) { $sha } else { "main" }
    if ($ghOk) {
        Tool gh release create $tag @($assets | ForEach-Object { $_.File }) --repo $repo --target $target --title $title --notes-file $notesFile | Out-Host
        if ($LASTEXITCODE) { throw "gh release create failed" }
        Write-Host ""
        Write-Host "Done: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
    } else {
        $url = "https://github.com/$repo/releases/new?tag=$tag&target=main" +
               "&title=" + [uri]::EscapeDataString($title) + "&body=" + [uri]::EscapeDataString($notes)
        Start-Process $url
        Start-Process explorer.exe "`"$outDir`""
        Write-Host ""
        Write-Host "The GitHub page for the new release is open in your browser, already filled in." -ForegroundColor Green
        Write-Host "1. From the Explorer window (the 'out' folder), drag these files onto the 'Attach binaries' box:"
        foreach ($a in $assets) { Write-Host "     $(Split-Path -Leaf $a.File)" }
        Write-Host "2. Wait for every upload to finish, then click 'Publish release'."
    }
} finally {
    Pop-Location
}
