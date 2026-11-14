#!/usr/bin/env bash
# desplegar.sh · Sube el modelo y despliega (o actualiza) la pila de predicción.
#
#   export BUCKET=mi-bucket-del-dataset
#   export AWS_IOT_ENDPOINT=xxxxxxxx-ats.iot.us-east-1.amazonaws.com
#   ./infra/desplegar.sh modelo_fc.json            # pila + modelo
#   ./infra/desplegar.sh modelo_fc.json --solo-modelo   # cambia el modelo sin tocar la pila
#
# Requiere AWS CLI v2 configurado (aws configure) con permisos de CloudFormation, IAM,
# Lambda, DynamoDB, IoT, Logs y S3. No guarda nada sensible en el repositorio.
set -euo pipefail

MODELO="${1:?uso: desplegar.sh modelo_fc.json [--solo-modelo]}"
: "${BUCKET:?define BUCKET}"
: "${AWS_IOT_ENDPOINT:?define AWS_IOT_ENDPOINT (aws iot describe-endpoint --endpoint-type iot:Data-ATS)}"
REGION="${AWS_REGION:-us-east-1}"
PILA="${PILA:-pulsoseguro-prediccion}"
LLAVE="modelos/modelo_fc.json"
AQUI="$(cd "$(dirname "$0")" && pwd)"

echo "▶ Verificando el modelo…"
python3 - "$MODELO" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
z = lambda e: m["intercepto"] + sum(w*(x-mu)/sd for w, x, mu, sd in zip(m["coef"], e["f"], m["media"], m["escala"]))
assert all(abs(max(-m["delta_max"], min(m["delta_max"], z(e))) - e["delta"]) < 1e-3 for e in m["ejemplos"])
v = m["validacion"]
print(f"  versión {m['version']} · usar={m['usar']} · MAE ridge {v['ridge']['mae']} vs media3 {v['media3']['mae']} ({v['sesiones']} sesiones)")
PY

echo "▶ Subiendo el modelo a s3://$BUCKET/$LLAVE"
aws s3 cp "$MODELO" "s3://$BUCKET/$LLAVE" --region "$REGION" --content-type application/json

if [[ "${2:-}" == "--solo-modelo" ]]; then
  echo "✔ Listo. La Lambda tomará el modelo nuevo en ≤ 5 min (revisa el ETag)."
  exit 0
fi

echo "▶ Empaquetando el código de la Lambda…"
EMPAQUETADO="$(mktemp -t pila-XXXX.yaml)"
aws cloudformation package --region "$REGION" \
  --template-file "$AQUI/template.yaml" \
  --s3-bucket "$BUCKET" --s3-prefix artefactos \
  --output-template-file "$EMPAQUETADO" >/dev/null

echo "▶ Desplegando la pila $PILA…"
aws cloudformation deploy --region "$REGION" \
  --stack-name "$PILA" \
  --template-file "$EMPAQUETADO" \
  --capabilities CAPABILITY_IAM \
  --no-fail-on-empty-changeset \
  --parameter-overrides ModeloBucket="$BUCKET" IotEndpoint="$AWS_IOT_ENDPOINT" \
      ArchivarFeatures="${ARCHIVAR_FEATURES:-false}"

aws cloudformation describe-stacks --region "$REGION" --stack-name "$PILA" \
  --query "Stacks[0].Outputs" --output table
echo "✔ Listo. Prueba sin la pulsera:  python herramientas/simular_features.py dataset.csv --thing <TU_THING>"
