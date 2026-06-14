param(
    [string]$Port = "COM8",
    [int]$Baud = 115200,
    [string]$Env = "tdeck",
    [switch]$SkipUpload,
    [switch]$NoStubUpload,
    [switch]$SkipBuild,
    [string]$ArtifactDir,
    [switch]$NoResetAfterUpload,
    [switch]$Reset,
    [int]$UploadBaud = 115200,
    [int]$OpenTimeout = 60,
    [int]$BootTimeout = 45,
    [int]$CommandTimeout = 30,
    [switch]$NoExpect,
    [string[]]$Commands = @("id", "sys", "net", "rf", "stats", "wifi", "companion test")
)

$ErrorActionPreference = "Stop"

$args = @(
    "scripts/tdeck_smoke.py",
    "--port", $Port,
    "--env", $Env,
    "--baud", "$Baud",
    "--upload-baud", "$UploadBaud",
    "--open-timeout", "$OpenTimeout",
    "--boot-timeout", "$BootTimeout",
    "--timeout", "$CommandTimeout"
)

if ($SkipUpload) {
    $args += "--skip-upload"
}

if ($NoStubUpload) {
    $args += "--no-stub-upload"
}

if ($SkipBuild) {
    $args += "--skip-build"
}

if ($ArtifactDir) {
    $args += "--artifact-dir"
    $args += $ArtifactDir
}

if ($NoResetAfterUpload) {
    $args += "--no-reset-after-upload"
}

if ($Reset) {
    $args += "--reset"
}

if ($NoExpect) {
    $args += "--no-expect"
}

$args += "--commands"
$args += $Commands
python @args
exit $LASTEXITCODE
