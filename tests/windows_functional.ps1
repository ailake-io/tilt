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

function Wait-TiltPort([int]$Port) {
  for ($i = 0; $i -lt 60; $i++) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
      $client.Connect("127.0.0.1", $Port)
      return
    } catch {
      Start-Sleep -Milliseconds 100
    } finally {
      $client.Dispose()
    }
  }
  throw "servico tilt nao abriu a porta $Port"
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

$port = 8492
$temp = Join-Path $env:TEMP ("tilt-windows-" + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $temp | Out-Null
$stdout = Join-Path $temp "servico.out"
$stderr = Join-Path $temp "servico.err"
$service = $null
try {
  $startArgs = @{
    FilePath = $Bin
    ArgumentList = @(
      "servir", "tests/fixtures/servico_versao.tilt", "--porta", "$port", "--requisicoes", "3"
    )
    WorkingDirectory = (Get-Location).Path
    RedirectStandardOutput = $stdout
    RedirectStandardError = $stderr
    PassThru = $true
  }
  $service = Start-Process @startArgs
  Wait-TiltPort $port

  $v1 = Invoke-RestMethod -Uri "http://127.0.0.1:$port/v1/ping"
  $v2 = Invoke-RestMethod -Uri "http://127.0.0.1:$port/v2/ping"
  $legado = Invoke-RestMethod -Uri "http://127.0.0.1:$port/legado"
  if ($v1.versao -cne "v1" -or $v2.versao -cne "v2" -or $legado.versao -cne "legado") {
    throw "versionamento de rotas HTTP divergiu"
  }
  $service.WaitForExit()
  if ($service.ExitCode -ne 0) {
    throw "servico de versionamento terminou com codigo $($service.ExitCode)"
  }
} finally {
  if ($null -ne $service -and -not $service.HasExited) {
    Stop-Process -Id $service.Id -Force -ErrorAction SilentlyContinue
  }
  Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

Write-Output "windows_functional ok"
