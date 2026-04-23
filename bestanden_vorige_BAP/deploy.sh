#!/bin/bash
# Deployment script for BAP-protocol

echo "Starting deployment of BAP-protocol..."

# Step 1: Activate virtual environment
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
echo "Virtual environment activated."

sudo apt update
sudo apt install mosquitto -y

if ! sudo systemctl is-active --quiet mosquitto; then
        python -m registration_software.__make_identity__ false
        sudo cp resources/creds/server.* /etc/mosquitto/certs/
        sudo cp mosquitto/aclfile /etc/mosquitto/aclfile
        sudo cp mosquitto/passwd /etc/mosquitto/passwd
        sudo cp mosquitto/mosquitto.conf /etc/mosquitto/conf.d/mosquitto.conf
        sudo chown mosquitto:mosquitto /etc/mosquitto/certs -R
        sudo systemctl enable mosquitto.service
        sudo systemctl start mosquitto.service
        echo "Mosquitto started and configured."
    else
        echo "Mosquitto is already running."
    fi
    
echo "Deployment completed."