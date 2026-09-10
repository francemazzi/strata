function Resolve-RequiredFile {
  param([string]$FilePath, [string]$Label)
  if ([string]::IsNullOrWhiteSpace($FilePath) -or -not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
    throw "$Label is missing: $FilePath"
  }
  return (Resolve-Path -LiteralPath $FilePath).ProviderPath
}

function Get-PeInfo {
  param([string]$FilePath)
  $reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($FilePath))
  try {
    if ($reader.BaseStream.Length -lt 2 -or $reader.ReadUInt16() -ne 0x5a4d) { return $null }
    if ($reader.BaseStream.Length -lt 64) { throw "Truncated PE: $FilePath" }
    $reader.BaseStream.Position = 60
    $offset = $reader.ReadUInt32()
    if ($offset + 152 -gt $reader.BaseStream.Length) { throw "Invalid PE offset: $FilePath" }
    $reader.BaseStream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE header: $FilePath" }
    $machine = $reader.ReadUInt16()
    $reader.BaseStream.Position = $offset + 24
    $magic = $reader.ReadUInt16()
    if ($magic -notin @(0x10b, 0x20b)) { throw "Invalid PE optional header: $FilePath" }
    $directories = if ($magic -eq 0x20b) { 112 } else { 96 }
    $reader.BaseStream.Position = $offset + 24 + $directories + 32
    $certificateOffset = $reader.ReadUInt32()
    $certificateSize = $reader.ReadUInt32()
    return @{ Machine = $machine; HasCertificate = ($certificateOffset -ne 0 -or $certificateSize -ne 0) }
  } finally { $reader.Dispose() }
}

function Test-AuthenticodeSignature {
  param([string]$FilePath)
  & $script:SignTool verify /pa /all /q $FilePath *> $null
  return $LASTEXITCODE -eq 0
}

function Invoke-UnsignedFile {
  param([string]$FilePath)
  $cache = $null
  if ($env:STRATA_SIGNED_CACHE) {
    New-Item -ItemType Directory -Force -Path $env:STRATA_SIGNED_CACHE | Out-Null
    $cache = Join-Path $env:STRATA_SIGNED_CACHE ((Get-FileHash $FilePath).Hash + [System.IO.Path]::GetExtension($FilePath))
    if (Test-Path $cache) {
      if (-not (Test-AuthenticodeSignature $cache)) { throw 'Invalid signature in this job signing cache.' }
      Copy-Item -LiteralPath $cache -Destination $FilePath -Force
      return
    }
  }
  & $script:SignTool sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib $script:Dlib /dmdf $script:Metadata /d Strata $FilePath | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "SignTool failed: $FilePath" }
  if (-not (Test-AuthenticodeSignature $FilePath)) { throw "Signature verification failed: $FilePath" }
  if ($cache) { Copy-Item -LiteralPath $FilePath -Destination $cache }
}

function Invoke-SignableFile {
  param([string]$FilePath, [string]$DisplayPath, [switch]$OnlyVerify)
  $pe = Get-PeInfo $FilePath
  $extension = [System.IO.Path]::GetExtension($FilePath).ToLowerInvariant()
  if (-not $pe) {
    if ($extension -in @('.exe', '.dll', '.pyd')) { throw "Runtime file is not a PE: $FilePath" }
    return
  }
  if ($extension -notin @('.exe', '.dll', '.pyd')) { throw "Uncovered PE extension: $FilePath" }
  $signature = Get-AuthenticodeSignature -LiteralPath $FilePath
  $valid = Test-AuthenticodeSignature $FilePath
  $action = 'preserved'
  if (-not $valid) {
    if ($OnlyVerify) { throw "Unsigned or invalid Authenticode signature: $FilePath" }
    if ($pe.HasCertificate -or $signature.Status -ne 'NotSigned') {
      throw "Existing signature is invalid; rebuild the dependency instead of replacing its signature: $FilePath"
    }
    Invoke-UnsignedFile $FilePath
    $signature = Get-AuthenticodeSignature -LiteralPath $FilePath
    $action = 'signed'
  }
  $script:Records.Add([ordered]@{
    path = $DisplayPath
    sha256 = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    machine = ('0x{0:x4}' -f $pe.Machine)
    signer = $signature.SignerCertificate.Subject
    thumbprint = $signature.SignerCertificate.Thumbprint
    status = 'valid'
    action = $action
  })
}

function Invoke-SignablePath {
  param([string]$RootPath, [switch]$Recursive, [switch]$OnlyVerify, [string]$Prefix = '')
  $root = Get-Item -LiteralPath $RootPath -ErrorAction Stop
  if (-not $root.PSIsContainer) {
    Invoke-SignableFile $root.FullName ($Prefix + $root.Name) -OnlyVerify:$OnlyVerify
    return
  }
  $files = @(Get-ChildItem -LiteralPath $root.FullName -File -Recurse:$Recursive | Sort-Object FullName)
  foreach ($file in $files) {
    $relative = $file.FullName.Substring($root.FullName.TrimEnd('\', '/').Length + 1).Replace('\', '/')
    Invoke-SignableFile $file.FullName ($Prefix + $relative) -OnlyVerify:$OnlyVerify
  }
}

function Invoke-ZipVerification {
  param([string]$ZipPath)
  $tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('strata-verify-' + [guid]::NewGuid().ToString('N'))
  try {
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $tempRoot -Force
    $before = $script:Records.Count
    Invoke-SignablePath $tempRoot -Recursive -OnlyVerify -Prefix ((Split-Path $ZipPath -Leaf) + '/')
    if ($script:Records.Count -eq $before) { throw "ZIP contains no executable code: $ZipPath" }
  } finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
  }
}
