param(
  [Parameter(Mandatory = $true)]
  [string]$Bin
)

$ErrorActionPreference = "Stop"

function Invoke-Tilt([string[]]$Arguments) {
  $lines = & $Bin @Arguments 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "tilt $($Arguments -join ' ') falhou com codigo $LASTEXITCODE`n$($lines -join "`n")"
  }
  return (($lines -join "`n") + "`n")
}

$interp = Invoke-Tilt @("executar", "tests/fixtures/jit_inteiros.tilt")
$vm = Invoke-Tilt @("executar", "--vm", "tests/fixtures/jit_inteiros.tilt")
$jit = Invoke-Tilt @("executar", "--jit", "tests/fixtures/jit_inteiros.tilt")
if ($interp -cne $vm -or $interp -cne $jit) {
  throw "paridade interpretador/VM/JIT falhou`ninterp:`n$interp`nvm:`n$vm`njit:`n$jit"
}

$fallbackInterp = Invoke-Tilt @("executar", "tests/fixtures/nativo2.tilt")
$fallbackJit = Invoke-Tilt @("executar", "--jit", "tests/fixtures/nativo2.tilt")
if ($fallbackInterp -cne $fallbackJit) {
  throw "fallback JIT para tipos fora do subconjunto divergiu"
}

[void](Invoke-Tilt @("checar", "exemplos/soma.tilt"))
[void](Invoke-Tilt @("executar", "exemplos/resumo_vendas.tilt"))
[void](Invoke-Tilt @("executar", "exemplos/etl_delta.tilt"))

Write-Output "windows_functional ok"
