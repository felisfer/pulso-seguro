<#
.SYNOPSIS
  Sube el modelo y despliega (o actualiza) la pila de predicción de PulsoSeguro.
  Equivalente en PowerShell de desplegar.sh (funciona en Windows PowerShell 5.1 y PowerShell 7).

.EXAMPLE
  .\infra\desplegar.ps1 -Modelo modelo_fc.json -Bucket mi-bucket-del-dataset
  .\infra\desplegar.ps1 -Modelo modelo_fc_v2.json -Bucket mi-bucket -SoloModelo
  # Con la API del historial del dashboard (User Pool y App client del login):
  .\infra\desplegar.ps1 -Modelo modelo_fc.json -Bucket mi-bucket -UserPoolId us-east-2_AbCdEf123 `
      -ClientId 1a2b3c... -Origenes 'http://localhost:5173,https://mi-dashboard.example.com'

.NOTES
  - Requiere AWS CLI v2 configurado (aws configure) con permisos de CloudFormation, IAM,
    Lambda, DynamoDB, IoT, Logs y S3.
  - -Bucket e -IotEndpoint también se leen de $env:BUCKET y $env:AWS_IOT_ENDPOINT.
    Si no das el endpoint, se obtiene solo con 'aws iot describe-endpoint'.
  - -UserPoolId, -ClientId y -Origenes son opcionales: si das -UserPoolId se crea la API del
    historial (salida UrlApiHistorial = VITE_HISTORY_API_URL). Si no los das en una
    actualización, la pila conserva los valores anteriores.
  - Si Windows bloquea el script:  powershell -ExecutionPolicy Bypass -File .\infra\desplegar.ps1 ...
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Modelo,
    [string]$Bucket = $env:BUCKET,
    [string]$IotEndpoint = $env:AWS_IOT_ENDPOINT,
    [string]$Region = $(if ($env:AWS_REGION) { $env:AWS_REGION } else { 'us-east-2' }),
    [string]$Pila = $(if ($env:PILA) { $env:PILA } else { 'pulsoseguro-prediccion' }),
    [string]$UserPoolId,
    [string]$ClientId,
    [string]$Origenes,
    [switch]$ArchivarFeatures,
    [switch]$SoloModelo
)

$ErrorActionPreference = 'Stop'
$Llave = 'modelos/modelo_fc.json'
$Aqui = $PSScriptRoot

# Los comandos nativos (aws) no lanzan excepción al fallar en PowerShell 5.1:
# este envoltorio revisa $LASTEXITCODE y se detiene, como 'set -e' en bash.
function Invoke-Aws {
    $salida = & aws @args
    if ($LASTEXITCODE -ne 0) { throw "Falló: aws $($args -join ' ')" }
    return $salida
}

if (-not (Get-Command aws -ErrorAction SilentlyContinue)) {
    throw 'No encuentro AWS CLI. Instálalo: https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html'
}
if (-not $Bucket) { throw 'Indica -Bucket o define $env:BUCKET' }
if (-not (Test-Path $Modelo)) { throw "No existe el archivo de modelo: $Modelo" }
if ($UserPoolId -and -not $ClientId) { throw 'Con -UserPoolId también hace falta -ClientId (App client del dashboard)' }
if (-not $IotEndpoint) {
    $IotEndpoint = Invoke-Aws iot describe-endpoint --endpoint-type iot:Data-ATS --region $Region `
        --query endpointAddress --output text
    $IotEndpoint = "$IotEndpoint".Trim()
    Write-Host "  Endpoint IoT detectado: $IotEndpoint"
}

# ---------------------------------------------------------------- 1) verificar el modelo
Write-Host '> Verificando el modelo...' -ForegroundColor Cyan
$m = Get-Content $Modelo -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($e in $m.ejemplos) {
    $z = [double]$m.intercepto
    for ($i = 0; $i -lt $m.coef.Count; $i++) {
        $z += $m.coef[$i] * ($e.f[$i] - $m.media[$i]) / $m.escala[$i]
    }
    $z = [math]::Max(-$m.delta_max, [math]::Min($m.delta_max, $z))
    if ([math]::Abs($z - $e.delta) -ge 1e-3) {
        throw "El modelo no reproduce sus ejemplos (esperado $($e.delta), calculado $z)"
    }
}
$v = $m.validacion
Write-Host ("  version {0} | usar={1} | MAE ridge {2} vs media3 {3} ({4} sesiones)" -f `
    $m.version, $m.usar, $v.ridge.mae, $v.media3.mae, $v.sesiones)

# ---------------------------------------------------------------- 2) subir el modelo
Write-Host "> Subiendo el modelo a s3://$Bucket/$Llave" -ForegroundColor Cyan
Invoke-Aws s3 cp $Modelo "s3://$Bucket/$Llave" --region $Region --content-type application/json | Out-Host

if ($SoloModelo) {
    Write-Host 'OK. La Lambda tomará el modelo nuevo en 5 min o menos (revisa el ETag).' -ForegroundColor Green
    return
}

# ---------------------------------------------------------------- 3) empaquetar y desplegar
Write-Host '> Empaquetando el código de la Lambda...' -ForegroundColor Cyan
$empaquetado = Join-Path ([System.IO.Path]::GetTempPath()) ("pila-{0}.yaml" -f [guid]::NewGuid().ToString('N').Substring(0, 8))
try {
    Invoke-Aws cloudformation package --region $Region `
        --template-file (Join-Path $Aqui 'template.yaml') `
        --s3-bucket $Bucket --s3-prefix artefactos `
        --output-template-file $empaquetado | Out-Null

    Write-Host "> Desplegando la pila $Pila..." -ForegroundColor Cyan
    $archivar = if ($ArchivarFeatures) { 'true' } else { 'false' }
    $parametros = @("ModeloBucket=$Bucket", "IotEndpoint=$IotEndpoint", "ArchivarFeatures=$archivar")
    if ($UserPoolId) { $parametros += "CognitoUserPoolId=$UserPoolId", "CognitoClientId=$ClientId" }
    if ($Origenes) { $parametros += "OrigenesCors=$Origenes" }
    Invoke-Aws cloudformation deploy --region $Region `
        --stack-name $Pila `
        --template-file $empaquetado `
        --capabilities CAPABILITY_IAM `
        --no-fail-on-empty-changeset `
        --parameter-overrides @parametros | Out-Host

    Invoke-Aws cloudformation describe-stacks --region $Region --stack-name $Pila `
        --query 'Stacks[0].Outputs' --output table | Out-Host
}
finally {
    Remove-Item $empaquetado -ErrorAction SilentlyContinue
}

Write-Host 'OK. Prueba sin la pulsera:  python herramientas\simular_features.py dataset.csv --thing <TU_THING> ...' -ForegroundColor Green
