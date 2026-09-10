function Invoke-WindowsProcess {
  param([string]$Executable, [string]$Arguments, [int]$TimeoutMilliseconds = 180000)
  $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru -NoNewWindow
  try {
    # Cache the handle before waiting: Windows PowerShell 5.1 can otherwise lose
    # the exit code of a short-lived process returned by Start-Process.
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
      $process.Kill()
      throw "Process timed out: $Executable"
    }
    if ($process.ExitCode -ne 0) { throw "Process exited with $($process.ExitCode): $Executable" }
  } finally { $process.Dispose() }
}
