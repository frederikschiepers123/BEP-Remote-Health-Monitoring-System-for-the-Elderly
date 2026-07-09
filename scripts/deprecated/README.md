deploy.sh is an older version of a deployment script for the tablet, which would do the following:

- Updates Termux packages
- Installs required dependencies
- Installs MagicMirror dependencies
- Installs the MQTT package
- Installs the Termux:Boot startup script (start_magicmirror.sh)
- Copies the Fully Kiosk configuration file to Downloads
- Copies the APK to Downloads (if present)
- Optionally opens the APK installer
- Prints clear instructions for the remaining manual steps

The file "deployment instructions.pdf" contains the corresponding deployment instructions.