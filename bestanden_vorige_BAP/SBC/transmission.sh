#!/bin/bash

## Run this script to deploy the SBC device client, after it has been registered.

source "$WORK_DIR/.venv/bin/activate"

python -m device.__run_dep_client__