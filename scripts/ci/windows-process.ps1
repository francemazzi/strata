function Invoke-WindowsProcess {
  param([string]$Executable, [string]$Arguments, [int]$TimeoutMilliseconds = 180000)
  $process = [System.Diagnostics.Process]::new()
  $process.StartInfo.FileName = $Executable
  $process.StartInfo.Arguments = $Arguments
  $process.StartInfo.UseShellExecute = $false
  try {
    # Own the process handle from launch. Start-Process can lose the exit code
    # before returning a very short-lived process on Windows PowerShell 5.1.
    if (-not $process.Start()) { throw "Process did not start: $Executable" }
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
      $process.Kill()
      throw "Process timed out: $Executable"
    }
    if ($process.ExitCode -ne 0) { throw "Process exited with $($process.ExitCode): $Executable" }
  } finally { $process.Dispose() }
}
