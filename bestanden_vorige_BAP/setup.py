import os
import sys
import subprocess
import venv

# Configuration
VENV_DIR = "venv"
REQ_FILE = "requirements.txt"

def create_venv():
    if os.path.exists(VENV_DIR):
        print(f"Virtual environment '{VENV_DIR}' already exists.")
    else:
        print(f"Creating virtual environment in '{VENV_DIR}'...")
        venv.create(VENV_DIR, with_pip=True)

def install_requirements():
    if not os.path.exists(REQ_FILE):
        print(f"Warning: '{REQ_FILE}' not found. Skipping installation.")
        return

    if sys.platform == "win32":
        pip_exe = os.path.join(VENV_DIR, "Scripts", "pip")
    else:
        pip_exe = os.path.join(VENV_DIR, "bin", "pip")
    
    try:
        # Upgrade pip first
        subprocess.check_call([pip_exe, "install", "--upgrade", "pip"])
        # Install requirements
        subprocess.check_call([pip_exe, "install", "-r", REQ_FILE])
    except subprocess.CalledProcessError:
        print("Error: Failed to install dependencies.")

if __name__ == "__main__":
    create_venv()
    install_requirements()
    
    print(f"To activate the environment, run:")
    if sys.platform == "win32":
        print(f"{VENV_DIR}\\Scripts\\activate")
    else:
        print(f"source {VENV_DIR}/bin/activate")