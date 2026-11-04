# Installs Node.js LTS and Git via winget if they're missing.
$installed = $false

if (Get-Command node -ErrorAction SilentlyContinue) {
    Write-Host "Node.js $(node -v) already installed"
} else {
    winget install -e --id OpenJS.NodeJS.LTS --accept-source-agreements --accept-package-agreements
    $installed = $true
}

if (Get-Command git -ErrorAction SilentlyContinue) {
    git --version
} else {
    winget install -e --id Git.Git --accept-source-agreements --accept-package-agreements
    $installed = $true
}

if ($installed) {
    Write-Host "Something was just installed. Close and reopen VS Code so PATH picks it up, then run the task again."
    exit 1
}
