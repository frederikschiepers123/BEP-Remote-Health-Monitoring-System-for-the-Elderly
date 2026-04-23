#!/bin/bash
source .venv/bin/activate
QT_QPA_PLATFORM=xcb # system variables that help OpenCV with RTSP streams
export OPENCV_FFMPEG_CAPTURE_OPTIONS="rtsp_transport;tcp|stimeout;15000000|rw_timeout;15000000|max_delay;1000000"

# Check what has to be registered and run the corresponding registration process
read -p "Do you want to register SBC's and Sensor modules (S) or Camera's (C) or generate a CA certificate (A)? (S/C/A): " choice
if [[ "$choice" == "S" || "$choice" == "s" ]]; then
    echo "Deploying for SBC's and Sensor modules..."
    echo "Starting peripheral registration..."
    python -m registration_software.__init__ S false true false
elif [[ "$choice" == "C" || "$choice" == "c" ]]; then
    echo "Deploying for Camera's..."
    python -m registration_software.__init__ C true
elif [[ "$choice" == "A" || "$choice" == "a" ]]; then
    echo "Generating CA certificate..."
    python -m registration_software.__make_identity__ true
else
    echo "Invalid choice. Please run the script again and choose either S or C or A."
    exit 1
fi

# Visualise the registration table
cd resources || exit    
echo "Starting HTTP server on port 8000 to serve resources."
python -m http.server 8000 
