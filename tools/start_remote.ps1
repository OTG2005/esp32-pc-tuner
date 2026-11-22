param(
    [Parameter(Mandatory = $true)][string] $Target,
    [string[]] $AllowedEmail = @(),
    [switch] $CheckOnly
)
# Optional wrapper; use start_remote.py directly where .ps1 execution is disabled.
$arguments = @((Join-Path $PSScriptRoot 'start_remote.py'), '--target', $Target)
if ($CheckOnly) { $arguments += '--check-only' }
foreach ($email in $AllowedEmail) { $arguments += @('--allowed-email', $email) }
& python @arguments
exit $LASTEXITCODE
