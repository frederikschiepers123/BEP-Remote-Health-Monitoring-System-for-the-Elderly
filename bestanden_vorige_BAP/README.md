# Registering Medical Devices - Registering Medical Devices
This is the README file for the registration protocol that is the result of the Bachelor's Graduation Project. 
The contents of this module implement a protocol that can register different kinds of peripheral devices for a RMMS. In this README the contents of every software file will be described, as well as the workflow to register devices.

### This folder contains the following:
- `./papers:` A folder containing all papers and cites used in this Bachelor Afstudeer Project.
- `./registration_software:` The registration software for camera's, sensor modules and SBC's.
- `./sbc_software:` The registration and connection software for the SBC's.
- `./sensor_module_software:` The registration, connection and sensor software for the sensor modules. Also incluces hardware design files.
- `./sensor_module:` The registration, connection and sensor software for the sensor modules. Also incluces hardware design files.
- `./mosquitto` and `./resources`: Configuration files and certificates used by the software. 

 
### Table of Contents  
- [Registering Medical Devices - Registering Medical Devices](#registering-medical-devices---registering-medical-devices)
    - [This folder contains the following:](#this-folder-contains-the-following)
    - [Table of Contents](#table-of-contents)
  - [Settings](#settings)
  - [Run the Registration Program](#run-the-registration-program)
    - [Prerequisites](#prerequisites)
  - [Label generator:](#label-generator)
    - [How it works:](#how-it-works)
    - [How to use:](#how-to-use)
  - [Active LED Identification](#active-led-identification)
    - [How it Works](#how-it-works-1)
    - [How to Use](#how-to-use-1)
    - [Camera Calibration](#camera-calibration)
  - [Sensor Module and SBC Registration:](#sensor-module-and-sbc-registration)
    - [How it Works](#how-it-works-2)
    - [How to use](#how-to-use-2)
  - [Camera registration:](#camera-registration)
    - [How it works:](#how-it-works-3)
    - [How to use:](#how-to-use-3)
      - [Camera Registration Settings](#camera-registration-settings)
  - [Sensor Module Tools](#sensor-module-tools)
    - [How it works:](#how-it-works-4)
    - [How to use:](#how-to-use-4)
      - [Flash + Restore](#flash--restore)
      - [Backup from device](#backup-from-device)
      - [Upload to device](#upload-to-device)

## Settings
All settings can be found in the file: `registration_software/common/config.py`. Settings are grouped using dataclasses to maximize readability. Settings are imported using: `from registration_software.common.config import settings`.

## Run the Registration Program
To run the registration program, the user can run the following command from an ubuntu terminal, which is working in the top level of this folder. This is the folder in which the `deploy.sh` is located:\
`sudo ./deploy.sh`\
After running this command, a virtual environment and mosquitto broker are set up and ready to use. Run `sudo ./registration.sh` from the same folder to actually run registration, then follow the prompts that are given by the scripts. When registration is finished, a clean table can be found in your browser, as mentioned by the prompts. The json file that contain the table can be found in `registration_software/resources/device_registry.json`

### Prerequisites
To ensure correct MQTT operation, a certificate authority should be provided. This can be done manually by placing a CA certificate and key in `registration_software/creds`. If no external CA certificate is required, a self-signed one can be created by choosing "A" after running the `./deploy.sh`. For now a certificate and key have been placed for every device, to make them ready to use. In the future these can be removed or overwritten if required. Make sure all devices have the same CA in their file tree.

## Label generator:
The Label generator can be found in: `registration_software/labels/label_generator.py`. It consist of a single Class called LabelGenerator(). 

### How it works:
The label generator generates unique labels. These labels will then be encoded into a QR code together with a signature
generated based an a private key and the label. These QR codes will then be saved as a .png file. The labels will also 
be saved separately to keep track of already existing labels.

### How to use:
In order to generate a number of labels first an instance of the class has to be created.
Afterwards `your_instance.generate(amount=, position=)` can be called.

Here `amount` is an integer that stands for the amount of labels/QR codes you want to create.

`position` is also an integer and determines the position of the label relative to the QR code.\
If not specified otherwise `position` is set to `1`. In this case the label will be placed directly above the QR code.\
If `position` is set to `2` the label will be placed to the right of the QR code.\
If `position` is set to `3` the label will be placed to the right of the QR code.

## Active LED Identification
The active LED identification used for registering sensor modules and SBS's is wrapped in a class `ActiveIdentificationTracker` in the file: `registration_software/identification/active_identification.py`. It combines video streaming, QR code detection, visualization and the LED identification algorithm to keep track devices in the camera FOV.

Note: active LED identification is already implemented for SBC and sensor module registration and does not need to be called separately.

### How it Works
The tracker is the device manager used to track and update devices in the FOV of the registration camera. It uses an output queue to communicate events with the registration software.

**The following components are used by the tracker:**
- `File/NetworkStreamer` found in `registration_software/interface/streamers.py`:\
    Either the file or network-streamer can be used to stream video from a source. Used as a live streamer for real-time data processing.

- `QrCodeDetector` found in `registration_software/identification/processing/qr_detector.py`:\
    The QR code detector used to detect, decode and verify QR codes in the video frame. Used as a seperate process to not interfere with the main thread. events are handled asynchronously.

- `LEDDetector` found in `registration_software/identification/processing/led_detector.py`:\
    The core of the LED identification algorithm. Used as part of the main thread every frame for real-time data processing.

- `Visualizer` found in `registration_software/interface/visualizer.py`:\
    The class handling the feedback window to give feedback to the user. Used as a seperate process to not interfere with the tracker. Is updated every frame for real-time data visualization.

- `DeviceState` found in `registration_software/models/device.py`:\
    A dataclass representing a device being tracked. Contains all data about the device necessary for the tracker.

### How to Use
The first step to use active identification is making sure the settings are set correctly in the config file under: `QrConfig`, `CameraConfig` and `ActiveIDConfig`. Make sure the QR code and active identification settings are set correctly. Change the camera settings to match the camera you are using. Use the camera calibration tool (Explained below) if you wish to use it.

To use the ActiveIdentificationTracker, it must first be initialized and started, then polled for events. The tracker should be initialized using the helper function `get_tracker()` from `registration_software/identification/__init__.py`. 
```
get_tracker(manual_source: str | int = None) -> ActiveIdentificationTracker:
    manual_source: (Optional) Overrides the camera source defined in the settings. It can be an integer (for a USB camera) or a string (for a file path or URL).
```

Since the tracker uses threading, it must be started to begin processing video frames using `tracker_object.start()`.

The tracker communicates results via a queue. Use the `get_events()` method to check for new updates. This returns `None` if the queue is empty or a dictionary containing the event type and device label. **Event Types:**
    
    IdentificationEvent.DEVICE_FOUND: A valid QR code has been detected and added to the tracking list.

    IdentificationEvent.IDENTIFIED: The LED blinking pattern of a device has been verified. The device status is updated to DeviceStatus.IDENTIFIED.

When the identification process is complete, call tracker_object.stop() to close the video stream and terminate the processing thread.

### Camera Calibration
The active identification estimates distance between the camera and the devices and uses it to give feedback to the user. This feedback is useful for correct camera and device placement. **The steps:**
1. Update the settings with the correct camera specifications. This includes:
    - IP Address. Set to None for auto discovery (time consuming).
    - Discovery Port (Default: 80 for http discovery on ONVIF cameras).
    - Username and Password.
    - Focal Length.
    - Sensor Width and Height.
2. Run the calibration tool using `run_camera_calibration_tool()` found in `registration_software/tools/calibrate.py` and follow the instructions.
3. Update the settings with distance constant, near limit and far limit given by the calibration tool.
4. The software now estimates the distance between the camera and devices and gives feedback to the user. The Visualizer will show if a device is too close or too far, as well as display the estimated distance.

Note: to disable the distance estimation, simply set the near and far limit to 0.0 and 'inf' respectively. The Visualizer will still show the distance estimation, but this can be ignored.

## Sensor Module and SBC Registration:
The algorithm for registering devices using MQTT can be found in `registration_software/registration/registration_client.py`. This makes use of a single class that first collects devices on the network, and then runs them all through the same cycle.

### How it Works
The `RegistrationClient` class runs through three phases. In the first phase it collects device that want to register by broadcasting periodic requests. In the next phase it checks authenticity of devices with a nonce challenge as well as finding out if their LED turns on in the correct moment. Based on these challenges it assigns labels and credentials. In the final phase it verifies if all devices are fully registered by sending checkups and requesting a final LED response. \

### How to use
To use the class it needs to be initialised. The three phase pipeline within the class is started automatically. During this initialisation a few variables need to be passed. \
**Variables:**
`CAManager: object`: handles anything related to certificates, meaning the generation and signing of them. Can be found in `registration_software/registration/ca_manager.py`.\
`DeviceRegistry: object`: handles the registry table file, can append to that and make sure there are no double entries. Can be found in `registration_software/common/registry`. \
`ActiveIdentifationTracker: object`: tracks LEDs that blink during registration. This is the class that has been explained earlier.\
`manual: bool`: gives the user the ability to manually fill in labels. \
`auto: bool`: determines if the script waits for a final user input in the third phase or just continues. \

## Camera registration:
The camera registration algortihm can be found in 
`registration_software/identification/passive_identification.py`, it consist of a single Class called `Cam_register()`. 

### How it works:
The `Cam_register()` function compares the translations from all secondary camera to a to the main camera and vice versa. By matching translations it assigns the correct label to the correct camera, UUID, etc. 

### How to use:
Before starting the registration algorithm the setup is important. Place all the camera's on a straight line across from one camera. This camera will be used as the main camera, all other cameras will be called secondary cameras. Make sure all the secondary cameras are facing the main camera and that main camera is facing the secondary cameras. Make sure all the cameras are turned on, connected to the same wifi network and that they have the same onvif credentials. Next, fill in the settings in the config file. All the settings that are of importance for `Cam_register()` can be found in the config file: `registration_software/common/config.py`

#### Camera Registration Settings
The meaning of each variable is also explained here. 

After specifying the variables and settings needed for your use case. In order to use the Cam_register you have to create an instance of the class and call `your_instance.start()`.\
When runnin the deploy script this will be done automatically.\
However there are a couple of variables that can be passed when creating the instance of your class. For most use cases these variables can be left untouched. If the setup and setting in the config file are correct the algorithm should be ready for use.\

But if you do want to change these variables it can be done in
BAP-protocol/registration_software/\_\_init__.py under:\
    `det = Cam_register("set variables here")`


**These variables include:**\
`Cam_register(`
- `one_cam=`
- `change_password=`
- `password=`
- `save_results=`

`)`\
All variables are set to `False` if not specified otherwise, except for `password` which is set to `None` unless 
specified otherwise. \
\
`one_cam`: Normally the algorithm will assume there should be more than 1 camera in the FOV of the main camera. 
So if you want to use this algorithm when only using 1 secondary camera (so 2 cameras in total), `one_cam=` must be set to `True`.\
\
`change_password` is used to change the ONVIF device credential of the succesfully registered cameras. 
Because changing the ONVIF credentials of a device using python is not always succesfull/allowed depending on the manufacturer.
This variable is normally set to `False`. \
The algorithm is designed to generate a random password, if you prefer to use
your own password this can be done by passing it as a string to the variable `password`.\
Since not all devices support/allow changing device credentials using python, it is important to be cautious when setting 
this variable to `True`. 

`save_results` can be set to `True` if you want to save the QR code scanning results. This includes the frame recieved 
from the camera as well as the ouput of the QR code detector for that frame.\
`save_results` is mainly intended give insight into the output of the cameras. This may be usefull whan a specific 
camera does not seem to produce any results. The results are saved to a file named after the IP address of each camera.

## Sensor Module Tools
The sensor module tools can be found at: `Sensor_module/tools/dist/ucontroller_tools.exe`.\
This exe file includes tools for flashing the device, uploading files to the device and backing up the device.

>[!warning]
>The backup tools only work when the .exe file has the correct permissions, thus check the folder you want to upload your backup to and/or give the executable permission for controlled folder access.

### How it works:
The sensor module tool makes use of mpremote to access the REPL of the microcontroller from the computer in order to manage the files on the microcontroller and upload files from the computer or download files from the microcontroller. \
For flashing it uses the esptool to flash the microcontroller firmware to the memory of the microcontroller. This flashing is done by using binary(.bin) files.

### How to use:
Launch the exe file by double clicking the it. When it has started there will be three tabs with three different functionalities: flash + restore, backup from device, upload to device.

#### Flash + Restore
In order to flash a microcontroller you first want to put it in boot mode.\
The microcontroller is put into boot mode with the following steps:\
1. hold the boot button(The left button on the front of the Lilygo TQ-T Pro)
2. press the reset button(little black button on the left side of the microcontroller)
3. release the boot button.
Now you can press the button `Refresh ports` to refresh all the input ports, and select the correct `Flash Port` from its dropdown menu.\
Select the correct binary(.bin) at `Firmware .bin` the file is found in `Sensor_module/bin_files/T-QT-Pro-firmware_4mb.bin` 

>[!Note]
>This binary file is specifically for the Lilygo TQ-T Pro with 4mb of memory and 2mb of PSRAM.

Select the correct `Restore folder` next the folder is `Sensor_module/microcontroller_files/` in our project files.\
The next section presents different settings for flashing the microcontroller: 
 - `Flash firmware` enables the flashing of the firmware specified atif not it just uploads the files. 
 - `Erase flash before writing` enables the abbility to erase the previous flash before writing the new firmware to the microcontroller.
 - `Restore files after flash` enables the writing of the files within the selected `Restore folder`.
 - `Reset after restore` enables you to reset the microcontroller at the end of the process.
the standard baud rate is 460800 Hz and the standard waiting time for the REPL port is 30 seconds.

Click the button `Run Flash + Restore` to let the program run.\
the previous firmware will be removed and the new firmware will be uploaded.\
Halfway through running the program the flash will be completed and the executable will look for the REPL port of the microcontroller.\
The microcontroller needs to be reset in order to exit boot mode and then the REPL port will be detected automatically.\
All the files within the `Restore folder` are uploaded.\
The microcontroller resets itself when the upload is finished.

#### Backup from device 
Select the correct `REPL port` this is the COM port the microcontroller is connected to.\
choose the `Base folder` for the device backup.\
Click `Run Backup` to backup all the files from the microcontroller.

#### Upload to device 
Select the correct `REPL port` this is the COM port the microcontroller is connected to.\
Select if you want to upload a single file or directory.\
Select the corresponding path for the directory or file.\
Input the destination for the directory or file at `Remote path (folder or file)`.\
Select the options you want:
 - `Clear destination folder first (folder upload only)` removes the destination first and creates a new folder with all the selected directory you want to upload. tThis works only if you want to upload a whole directory.
 - `Reset after upload` resets the microcontroller after the files have been uploaded.

Click `Upload to device` to upload the selected file(s) to the microcontroller.
