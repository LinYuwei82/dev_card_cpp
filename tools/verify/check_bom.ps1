# 与 check_bom.py 共用同一份判据实现。
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
python "$here\check_bom.py" --root $root @args
exit $LASTEXITCODE
