#!/data/data/com.termux/files/usr/bin/bash

# Updates Termux packages
# Installs required dependencies
# Installs MagicMirror dependencies
# Installs the MQTT package
# Installs the Termux:Boot startup script
# Copies the Fully Kiosk configuration file to Downloads
# Copies the APK to Downloads (if present)
# Optionally opens the APK installer
# Prints clear instructions for the remaining manual steps

set -e

echo "======================================="
echo " Remote Health Monitoring Deployment"
echo "======================================="
echo ""

########################################
# Storage access
########################################

echo "Requesting storage access..."
echo "If Android asks for permission, press ALLOW."
echo ""

termux-setup-storage

sleep 5

########################################
# Install dependencies
########################################

echo "Updating Termux packages..."

pkg update -y
pkg upgrade -y

echo "Installing required packages..."

pkg install -y \
    git \
    nodejs

########################################
# Install MagicMirror
########################################

echo ""
echo "Installing MagicMirror dependencies..."

cd MagicMirror

npm install

echo ""
echo "Running MagicMirror installer..."

node --run install-mm

########################################
# Install MQTT dependency
########################################

echo ""
echo "Installing MQTT dependency..."

cd modules/MMM-CustomMQTTBridge

npm install mqtt

cd ../../..

########################################
# Install boot script
########################################

echo ""
echo "Installing boot script..."

mkdir -p ~/.termux/boot

cp boot/start_magicmirror.sh \
   ~/.termux/boot/start_magicmirror.sh

chmod +x ~/.termux/boot/start_magicmirror.sh

########################################
# Copy Fully Kiosk configuration
########################################

echo ""
echo "Installing Fully Kiosk configuration..."

mkdir -p ~/storage/downloads

cp fully/fully-settings.json \
   ~/storage/downloads/fully-settings.json

########################################
# Copy APK
########################################

if [ -f apk/appScreenControl.apk ]; then

    echo ""
    echo "Copying APK..."

    cp apk/appScreenControl.apk \
       ~/storage/downloads/appScreenControl.apk

    echo ""
    echo "Launching APK installer..."

    termux-open \
       ~/storage/downloads/appScreenControl.apk

else

    echo ""
    echo "APK not found:"
    echo "apk/appScreenControl.apk"

fi

########################################
# Finished
########################################

echo ""
echo "======================================="
echo " DEPLOYMENT COMPLETED"
echo "======================================="
echo ""
echo "Remaining manual steps:"
echo ""
echo "1. Install the APK if Android prompts."
echo ""
echo "2. Open Fully Kiosk Browser."
echo ""
echo "3. Import:"
echo "   Downloads/fully-settings.json"
echo ""
echo "4. Open Termux:Boot once (to allow system to start the app at boot)."
echo ""
echo "5. Reboot the tablet."
echo ""
echo "6. Verify that:"
echo "   - MagicMirror starts"
echo "   - Fully Kiosk launches"
echo "   - The mirror page is displayed"
echo ""
