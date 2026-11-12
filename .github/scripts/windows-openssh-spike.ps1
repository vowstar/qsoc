# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

param([Parameter(Mandatory)][string]$ResultPath)
$ErrorActionPreference = 'Stop'
$timer = [Diagnostics.Stopwatch]::StartNew()
$children = [Collections.Generic.List[object]]::new()
$probeDir = $null; $stage = 'platform'; $statePath = "$ResultPath.state"
$result = [ordered]@{ status = 'unavailable'; reason = 'not_started'; tools = @{}; read_ok = $false; write_blocked = $false; exec_forced_sftp = $false; cleanup_ok = $false }
function Save-State {
    @{ directory = $probeDir; children = @($children | ForEach-Object { @{ id = $_.Id; ticks = $_.Ticks } }) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$statePath.new" -Encoding utf8
    Move-Item -LiteralPath "$statePath.new" -Destination $statePath -Force
}
function Start-Child($file, [string[]]$arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($file)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true; $info.WorkingDirectory = $probeDir
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    foreach ($value in $arguments) { $info.ArgumentList.Add($value) }
    $process = [Diagnostics.Process]::Start($info)
    $entry = @{ Process = $process; Id = $process.Id; Ticks = $process.StartTime.ToUniversalTime().Ticks; Out = [IO.MemoryStream]::new(); Err = [IO.MemoryStream]::new() }
    $entry.OutTask = $process.StandardOutput.BaseStream.CopyToAsync($entry.Out); $entry.ErrTask = $process.StandardError.BaseStream.CopyToAsync($entry.Err)
    $children.Add($entry); Save-State; return $entry
}
function Run-Child($file, [string[]]$arguments, [byte[]]$inputBytes = @()) {
    $remaining = [Math]::Min(10000, 75000 - $timer.ElapsedMilliseconds)
    if ($remaining -le 0) { throw 'deadline' }
    $entry = Start-Child $file $arguments
    if ($inputBytes.Length) { $entry.Process.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length) }
    $entry.Process.StandardInput.Close()
    if (!$entry.Process.WaitForExit([int]$remaining)) { $entry.Process.Kill($true); throw 'deadline' }
    if (!$entry.OutTask.Wait(1000) -or !$entry.ErrTask.Wait(1000)) { throw 'pipe_timeout' }
    return @{ Code = $entry.Process.ExitCode; Bytes = $entry.Out.ToArray(); Error = [Text.Encoding]::UTF8.GetString($entry.Err.ToArray()) }
}
function Protect-File($path, $hostOnly = $false, $directory = $false) {
    $acl = if ($directory) { [Security.AccessControl.DirectorySecurity]::new() } else { [Security.AccessControl.FileSecurity]::new() }
    $acl.SetAccessRuleProtection($true, $false); $acl.SetOwner($(if ($hostOnly) { $adminSid } else { $userSid }))
    $allowed = if ($hostOnly) { @($systemSid, $adminSid) } else { @($userSid, $systemSid, $adminSid) }
    foreach ($sid in $allowed) {
        $rule = if ($directory) { [Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow') } else { [Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow') }
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $path -AclObject $acl
}
try {
    if (!$IsWindows -or $PSVersionTable.PSVersion.Major -lt 7) { throw 'requires_windows_pwsh7' }
    $stage = 'discovery'; $tools = @{}
    $folders = @("$env:WINDIR/System32/OpenSSH", "$env:ProgramFiles/OpenSSH")
    $command = Get-Command sshd.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $folders += Split-Path -Parent $command.Source }
    foreach ($name in @('sshd', 'ssh', 'sftp', 'ssh-keygen')) {
        $tools[$name] = $folders | ForEach-Object { Join-Path $_ "$name.exe" } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        $result.tools[$name] = @{ available = [bool]$tools[$name]; origin = $(if (!$tools[$name]) { 'absent' } elseif ($tools[$name].StartsWith($env:WINDIR, [StringComparison]::OrdinalIgnoreCase)) { 'system32' } elseif ($tools[$name].StartsWith($env:ProgramFiles, [StringComparison]::OrdinalIgnoreCase)) { 'program_files' } else { 'path_server_folder' }) }
    }
    if (@('sshd', 'ssh', 'sftp', 'ssh-keygen').Where({ !$tools[$_] }).Count) { throw 'native_openssh_missing' }
    $account = $env:USERNAME.ToLowerInvariant()
    if ($account -notmatch '^[a-z0-9_.-]+$') { throw 'unsupported_runner_account' }
    $probeDir = Join-Path $env:RUNNER_TEMP ('qsoc-openssh-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $probeDir | Out-Null; Save-State
    $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $systemSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-18'); $adminSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $stage = 'temporary_acl'; Protect-File $probeDir $false $true
    $stage = 'key_generation'
    foreach ($name in @('host_key', 'client_key')) {
        if ((Run-Child $tools['ssh-keygen'] @('-q', '-t', 'ed25519', '-N', '', '-C', 'qsoc-spike', '-f', "$probeDir/$name")).Code) { throw 'key_generation_failed' }
    }
    Copy-Item -LiteralPath "$probeDir/client_key.pub" -Destination "$probeDir/authorized_keys"
    $stage = 'host_key_acl'; Protect-File "$probeDir/host_key" $true; Protect-File "$probeDir/client_key"; Protect-File "$probeDir/authorized_keys"
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    $posix = $probeDir.Replace('\', '/'); $config = "$probeDir/sshd_config"
    @("Port $port", 'ListenAddress 127.0.0.1', "HostKey `"$posix/host_key`"", "AuthorizedKeysFile `"$posix/authorized_keys`"", "AllowUsers $account", 'AuthenticationMethods publickey', 'PasswordAuthentication no', 'PubkeyAuthentication yes', 'PermitTTY no', 'AllowTcpForwarding no', 'AllowAgentForwarding no', 'Subsystem sftp internal-sftp', "ForceCommand internal-sftp -R -d `"$posix`"", 'LogLevel ERROR') | Set-Content -LiteralPath $config -Encoding ascii
    $stage = 'config_validation'
    if ((Run-Child $tools.sshd @('-t', '-f', $config)).Code) { throw 'config_or_host_key_rejected' }
    $stage = 'server_start'; $server = Start-Child $tools.sshd @('-D', '-e', '-f', $config); $ready = $false
    for ($attempt = 0; $attempt -lt 40 -and !$server.Process.HasExited; ++$attempt) {
        $tcp = [Net.Sockets.TcpClient]::new()
        try { if ($tcp.ConnectAsync('127.0.0.1', $port).Wait(100)) { $ready = $tcp.Connected } } catch {} finally { $tcp.Dispose() }
        if ($ready) { break }; Start-Sleep -Milliseconds 100
    }
    if (!$ready) { throw 'server_unavailable' }
    $hostPublic = (Get-Content -LiteralPath "$probeDir/host_key.pub" -Raw).Trim()
    "[127.0.0.1]:$port $hostPublic" | Set-Content -LiteralPath "$probeDir/known_hosts" -Encoding ascii
    '' | Set-Content -LiteralPath "$probeDir/client_config" -Encoding ascii
    $common = @('-F', "$probeDir/client_config", '-i', "$probeDir/client_key", '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'IdentityAgent=none', '-o', 'StrictHostKeyChecking=yes', '-o', "UserKnownHostsFile=`"$posix/known_hosts`"", '-o', 'GlobalKnownHostsFile=NUL', '-o', 'ConnectTimeout=3', '-o', 'ConnectionAttempts=1', '-o', 'ClearAllForwardings=yes')
    $sample = 'qsoc-read-' + [guid]::NewGuid().ToString('N'); $sample | Set-Content -LiteralPath "$probeDir/sample.txt" -Encoding ascii
    $stage = 'sftp_read'; 'get sample.txt downloaded.txt' | Set-Content -LiteralPath "$probeDir/read.batch" -Encoding ascii
    $read = Run-Child $tools.sftp ($common + @('-P', "$port", '-b', "$probeDir/read.batch", "$account@127.0.0.1"))
    if ($read.Code -or !(Test-Path -LiteralPath "$probeDir/downloaded.txt") -or (Get-FileHash -LiteralPath "$probeDir/downloaded.txt" -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath "$probeDir/sample.txt" -Algorithm SHA256).Hash) { throw 'sftp_read_failed' }; $result.read_ok = $true
    $stage = 'sftp_write'; 'put sample.txt must-not-write.txt' | Set-Content -LiteralPath "$probeDir/write.batch" -Encoding ascii
    $write = Run-Child $tools.sftp ($common + @('-P', "$port", '-b', "$probeDir/write.batch", "$account@127.0.0.1"))
    if (!$write.Code -or $write.Error -notmatch '(?i)permission denied' -or (Test-Path -LiteralPath "$probeDir/must-not-write.txt")) { $result.status = 'contract_failed'; throw 'readonly_not_proven' }; $result.write_blocked = $true
    $stage = 'exec'; $exec = Run-Child $tools.ssh ($common + @('-T', '-p', "$port", '-l', $account, '127.0.0.1', 'cmd.exe /d /c echo QSOC_EXEC_MUST_NOT_RUN')) ([byte[]]@(0, 0, 0, 5, 1, 0, 0, 0, 3))
    if ($exec.Code -eq 255) { throw 'exec_connection_failed' }
    if ($exec.Bytes.Length -lt 9 -or $exec.Bytes[4] -ne 2 -or $exec.Bytes[5] -ne 0 -or $exec.Bytes[6] -ne 0 -or $exec.Bytes[7] -ne 0 -or $exec.Bytes[8] -ne 3 -or [Text.Encoding]::UTF8.GetString($exec.Bytes).Contains('QSOC_EXEC_MUST_NOT_RUN')) { $result.status = 'contract_failed'; throw 'exec_not_forced_to_sftp' }
    $result.exec_forced_sftp = $true; $result.status = 'available'; $result.reason = 'readonly_loopback_proven'
} catch {
    $result.exception_type = $_.Exception.GetType().Name; $result.native_error = $_.Exception.NativeErrorCode
    $code = $_.Exception.Message; $result.reason = if ($code -match '^[a-z0-9_]+$') { $code } else { 'permission_or_runtime_failure' }; $result.stage = $stage
} finally {
    $clean = $true
    foreach ($entry in $children) {
        try { if (!$entry.Process.HasExited) { $entry.Process.Kill($true); if (!$entry.Process.WaitForExit(2000)) { $clean = $false } } } catch { $clean = $false }
    }
    $diagnostic = [Text.Encoding]::UTF8.GetString([byte[]]@($children | ForEach-Object { $_.Err.ToArray() }))
    $result.diagnostic = if ($result.status -eq 'available') { 'none' } elseif ($diagnostic -match '(?i)privilege|access.*denied|permission.*denied|logon failure') { 'privilege_or_acl' } elseif ($diagnostic -match '(?i)host.?key|bad owner|too open') { 'key_or_acl' } elseif ($diagnostic -match '(?i)bad configuration|unsupported option') { 'config_unsupported' } else { 'none_classified' }
    if ($probeDir) { try { Remove-Item -LiteralPath $probeDir -Recurse -Force } catch { $clean = $false } }
    $result.cleanup_ok = $clean; $result.elapsed_ms = $timer.ElapsedMilliseconds
    if (!$clean) { $result.status = 'contract_failed'; $result.reason = 'cleanup_failed' } else { Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResultPath -Encoding utf8
    $result | ConvertTo-Json -Depth 4 -Compress | Write-Output
}
if ($result.status -eq 'contract_failed') { exit 1 }
