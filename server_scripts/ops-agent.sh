#!/bin/bash
set -e

# Usage: ./ops.sh <INSTANCE_NAME> <PROJECT_ID>
# Example: ./ops.sh dev-server nd-exp-478509

# Project Configuration
PROJECT_ID=$2
INSTANCE_NAME=$1

if [[ -z "$INSTANCE_NAME" ]]; then
    echo "Error: INSTANCE_NAME not provided. Usage: $0 <INSTANCE_NAME>"
    exit 1
fi

# 1. Ensure you are logged in as a USER

echo "--------------------------------------------------------"
if ! gcloud auth list --filter=status:ACTIVE --format="value(account)" | grep -q "@"; then
echo "No active user account found. Please login as your @endee.io or personal account:"
gcloud auth login
fi

# 2. Capture your active user email to override the Service Account

ACTIVE_USER=$(gcloud auth list --filter=status:ACTIVE --format="value(account)")
echo "Forcing execution as: $ACTIVE_USER"

# 3. Load NDD_SERVER_ID from .env file

if [ -f .env ]; then
    # Safely load only NDD_SERVER_ID, avoiding issues with multi-line values
    export NDD_SERVER_ID=$(grep -E '^NDD_SERVER_ID=' .env | cut -d '=' -f2- | tr -d '"' | tr -d "'")
    echo "Loaded NDD_SERVER_ID: $NDD_SERVER_ID"
else
    echo "Error: .env file not found."
    exit 1
fi

if [[ -z "$NDD_SERVER_ID" ]]; then
    echo "Error: NDD_SERVER_ID not found in .env file."
    exit 1
fi

echo "--------------------------------------------------------"

# 4. Find instance ZONE using instance NAME

ZONE=$(gcloud compute instances list \
    --account="$ACTIVE_USER" \
    --project="$PROJECT_ID" \
    --filter="name=($INSTANCE_NAME)" \
    --format="value(zone)")

if [[ -z "$ZONE" ]]; then
    echo "Error: Could not find instance with name $INSTANCE_NAME in project $PROJECT_ID."
    exit 1
fi

echo "Found instance:"
echo "  Name: $INSTANCE_NAME"
echo "  Zone: $ZONE"

echo "--------------------------------------------------------"
echo "1. Installing Ops Agent on $INSTANCE_NAME..."

gcloud compute ssh "$INSTANCE_NAME" \
    --account="$ACTIVE_USER" \
    --project="$PROJECT_ID" \
    --zone="$ZONE" \
    --command="curl -sSO https://dl.google.com/cloudagents/add-google-cloud-ops-agent-repo.sh && sudo bash add-google-cloud-ops-agent-repo.sh --also-install"

echo "--------------------------------------------------------"
echo "2. Injecting NDD_SERVER_ID into Ops Agent Config..."

REMOTE_CONFIG="
logging:
  receivers:
    docker_logs:
      type: files
      include_paths:
        - /var/lib/docker/containers/*/*-json.log
  processors:
    parse_docker_json:
      type: parse_json
      time_key: time
      time_format: \"%Y-%m-%dT%H:%M:%S.%LZ\"
    add_NDD_SERVER_ID:
      type: modify_fields
      fields:
        labels.NDD_SERVER_ID:
          static_value: '$NDD_SERVER_ID'
  service:
    pipelines:
      default_pipeline:
        receivers: [docker_logs]
        processors: [parse_docker_json, add_NDD_SERVER_ID]
"

gcloud compute ssh "$INSTANCE_NAME" \
    --account="$ACTIVE_USER" \
    --project="$PROJECT_ID" \
    --zone="$ZONE" \
    --command="echo '$REMOTE_CONFIG' | sudo tee /etc/google-cloud-ops-agent/config.yaml > /dev/null && sudo service google-cloud-ops-agent restart"

echo "--------------------------------------------------------"
echo "3. Updating Log Retention to 180 days..."

# This is the command that usually fails due to 'Insufficient Scopes'
# We bypass it by specifying the --account flag directly.

gcloud logging buckets update _Default \
    --account="$ACTIVE_USER" \
    --project="$PROJECT_ID" \
    --location=global \
    --retention-days=180

echo "--------------------------------------------------------"
echo "Success! All logs will now include the label: NDD_SERVER_ID = $NDD_SERVER_ID"
