#!/bin/bash

# This is the deploy script used in the RMMS deployment process.
# It sets up the necessary environment and starts the SBC application.

set -euo pipefail
CWD="$(dirname "$(realpath $0)")"
echo "Starting RMMS deployment script in $CWD"

# Update and install required packages
apt update
apt upgrade -y
apt install mosquitto -y

# Step 1: Set up Python virtual environment
python3 -m venv "$CWD/.venv"
source "$CWD/.venv/bin/activate"
pip install --upgrade pip
pip install -r "$CWD/requirements.txt"
echo "Python virtual environment set up."

# Remove any existing ExecStart line and insert the new one under [Service] section
sed -i '/^ExecStart=/d' $CWD/base/RMMS.service
sed -i "/^\[Service\]/a ExecStart=$CWD/registration.sh" $CWD/base/RMMS.service 
cp $CWD/base/RMMS.service /etc/systemd/system/RMMS.service

# Actually start the service
systemctl daemon-reload
systemctl enable RMMS.service
systemctl start RMMS.service
systemctl status RMMS.service
