#!/bin/bash

# SBC/device.sh
# This script is used to initialize and run the device client process
# for the Single Board Computer (SBC) in the BAP protocol system.
# It sets up the necessary environment and starts the device client.

# Create a working directory for the SBC device
WORK_DIR="$(dirname "$(realpath $0)")"
echo "Starting SBC device script in $WORK_DIR"
cd "$WORK_DIR" || { echo "Failed to change directory to $WORK_DIR"; exit 1; }
BASE_DIR="$WORK_DIR/base"

# Set environment variables for the registration client
source "$WORK_DIR/.venv/bin/activate"
# Run the device client
FILE="$WORK_DIR/data/status.json"
if [ -f "$FILE" ]; then
    echo "Device already registered. Starting client..."    # If the device is registered, run the deployment client
    # Normally would run DepClient here, in the testing phase, the user should run the deployment client manually
    echo "User, please run the deployment client manually."
    #python3 -m device.__run_dep_client__
    echo "Client process finished. Exiting."
else
    echo "Device not registered. Starting registration process..."  # If not registered, run the registration client
    python3 -m device.__run_client__ "$(cat /etc/machine-id)"
    # Copy necessary files for Mosquitto MQTT broker
    cp $BASE_DIR/server.* /etc/mosquitto/certs        
    cp $BASE_DIR/ca.crt /etc/mosquitto/certs
    cp $BASE_DIR/aclfile /etc/mosquitto/aclfile
    cp $BASE_DIR/mosquitto.conf /etc/mosquitto/mosquitto.conf
    chown mosquitto:mosquitto /etc/mosquitto/certs -R
    # Restart or start the Mosquitto service to apply changes
    systemctl stop mosquitto.service
    systemctl enable mosquitto.service
    systemctl start mosquitto.service
    echo "Registration process finished. Exiting."
fi

exit 0
