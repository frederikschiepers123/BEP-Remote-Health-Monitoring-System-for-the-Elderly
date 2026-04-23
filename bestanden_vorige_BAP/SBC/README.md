### SBC software
This folder contains all software files for the SBC.

## How it works
There are two classes, one for the registration of the SBC, and one for the deployment of the SBC. They can be found in `device/Client.py` and `device/DepClient.py` respectively. The `Client` class walks through all steps required for registering the SBC, the `DepClient` class connects to the sensor modules that give it measurement data.

## How to use
By running `./deploy.sh` an Ubuntu service is created that tries to register the SBC until it is registered. The service activates every time on startup. It runs the proper client dependent on whether or not it has been registered. For now, to get output data from the `DepClient`, the `./transmission.sh` should be ran. When this output is handled automatically, the service can take care of it. To handle registration manually and get all feedback from the script in the terminal, stop the service with `systemctl stop RMMS` and run `./registration.sh`

## File structure
`device/led_control.py` controls the LED and makes it blink. \
`device/Client.py` contains the registration client. \
`device/DepClient.py` contains the deployment client. This is were normally the connection to the database server would be made. Where exactly can be seen in the comments. \
`device/config.py` contains the settings. \
`device/utils.py` contains the utility functions. \
`device/__run_client__.py` and `device/__run_dep_client__.py` are used by the shell scripts to run the clients. \
The `base` folder contains the CA, the mosquitto configuration and the service.
The `data` folder will eventually contain the status.json file.