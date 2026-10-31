# Convert the text files under a folder to LF. The guest source is copied from a
# Windows working tree, and a checkout made before .gitattributes asked for LF still
# has CRLF, which breaks dkms.conf, Makefiles and shell scripts inside the guest.
param([Parameter(Mandatory = $true)][string]$Path)

Get-ChildItem -LiteralPath $Path -Recurse -File | ForEach-Object {
    $bytes = [IO.File]::ReadAllBytes($_.FullName)
    if ($bytes -notcontains 13 -or $bytes -contains 0) { return }   # no CR, or binary
    $out = New-Object IO.MemoryStream
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13 -and $i + 1 -lt $bytes.Length -and $bytes[$i + 1] -eq 10) { continue }
        $out.WriteByte($bytes[$i])
    }
    [IO.File]::WriteAllBytes($_.FullName, $out.ToArray())
}
