import sys
import os
import subprocess
import shutil
import platform
import urllib.request

# ================= CONFIGURATION =================
REPO_URL = "https://github.com/EndeeLabs/VectorDBBench.git"
REPO_DIR = "VectorDBBench"
PYTHON_VERSION = "3.11.9"
# =================================================

def run_command(command, shell=False, cwd=None):
    """Runs a shell command and exits on failure."""
    use_shell = shell
    # On Windows, list commands usually need shell=True to find executables
    if platform.system() == "Windows" and isinstance(command, list):
        use_shell = True
        
    cmd_str = ' '.join(command) if isinstance(command, list) else command
    print(f"--> [EXEC]: {cmd_str}")
    
    try:
        subprocess.check_call(command, shell=use_shell, cwd=cwd)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e}")
        sys.exit(1)

def get_os_type():
    """Returns 'windows', 'macos', or 'linux'."""
    os_name = platform.system().lower()
    
    if "darwin" in os_name:
        return "macos"
    elif "win" in os_name:
        return "windows"
    return "linux"

def find_python311():
    """Finds python3.11 executable path cross-platform."""
    candidates = []
    
    if get_os_type() == "windows":
        candidates = ["py", "python", "python3.11"]
    else:
        # Check standard PATH first, then explicit locations
        candidates = ["python3.11", "/usr/bin/python3.11", "/usr/local/bin/python3.11", "/opt/homebrew/bin/python3.11"]

    for cmd in candidates:
        path = shutil.which(cmd)
        if path:
            try:
                # Verify it is actually 3.11
                # We use --version to check the actual output
                ver = subprocess.check_output([path, "--version"]).decode().strip()
                if "3.11" in ver:
                    return path
            except:
                continue
    return None

def install_linux_strategy():
    """Installs Python 3.11 on Linux (Ubuntu PPA or Debian Source)."""
    print("\n[Linux] Python 3.11 not found. Determining installation strategy...")
    
    if not shutil.which("apt-get"):
         print("Error: This script requires 'apt-get' (Debian/Ubuntu/Mint/Kali).")
         sys.exit(1)

    # 1. Detect Ubuntu (Use PPA for speed)
    is_ubuntu = False
    try:
        if os.path.exists("/etc/os-release"):
            with open("/etc/os-release") as f:
                if "ubuntu" in f.read().lower():
                    is_ubuntu = True
    except:
        pass

    run_command("sudo apt-get update", shell=True)

    if is_ubuntu:
        print("Detected Ubuntu. Attempting PPA install...")
        try:
            run_command("sudo apt-get install -y software-properties-common", shell=True)
            run_command("sudo add-apt-repository -y ppa:deadsnakes/ppa", shell=True)
            run_command("sudo apt-get update", shell=True)
            run_command("sudo apt-get install -y python3.11 python3.11-venv python3.11-dev", shell=True)
            return
        except Exception as e:
            print(f"Ubuntu PPA failed ({e}). Falling back to source build.")

    # 2. Debian/Fallback Strategy (Source Build)
    print("Detected Debian/Other. Using Source Build (Robust Method)...")
    
    # Install Build Dependencies
    deps = [
        "wget", "build-essential", "zlib1g-dev", "libncurses5-dev", 
        "libgdbm-dev", "libnss3-dev", "libssl-dev", "libreadline-dev", 
        "libffi-dev", "libsqlite3-dev", "libbz2-dev", "pkg-config"
    ]
    run_command(f"sudo apt-get install -y {' '.join(deps)}", shell=True)
    
    # Download & Build
    tarball = f"Python-{PYTHON_VERSION}.tgz"
    url = f"https://www.python.org/ftp/python/{PYTHON_VERSION}/{tarball}"
    
    if not os.path.exists(tarball):
        run_command(f"wget {url}", shell=True)
    
    run_command(f"tar -xf {tarball}", shell=True)
    src_dir = f"Python-{PYTHON_VERSION}"
    
    # Configure & Install
    # --enable-optimizations is standard for production python builds
    run_command("./configure --enable-optimizations", shell=True, cwd=src_dir)
    nproc = subprocess.check_output("nproc", shell=True).decode().strip()
    run_command(f"make -j {nproc}", shell=True, cwd=src_dir)
    
    # CRITICAL: Use altinstall to avoid overwriting /usr/bin/python3
    run_command("sudo make altinstall", shell=True, cwd=src_dir)
    
    # Cleanup
    os.remove(tarball)
    run_command(f"sudo rm -rf {src_dir}", shell=True)

def install_macos_strategy():
    print("\n[macOS] Installing Python 3.11 via Homebrew...")
    if shutil.which("brew") is None:
        print("Error: Homebrew not found. Please install it from brew.sh")
        sys.exit(1)
    run_command("brew install python@3.11", shell=True)

def install_windows_strategy():
    print("\n[Windows] Installing Python 3.11 via Winget/Installer...")
    # Try Winget first (standard on Win 10/11)
    if shutil.which("winget"):
        try:
            run_command("winget install -e --id Python.Python.3.11", shell=True)
            return
        except:
            pass
    
    # Fallback to direct download
    installer = "python-3.11.9-amd64.exe"
    url = f"https://www.python.org/ftp/python/3.11.9/{installer}"
    print(f"Downloading {url}...")
    urllib.request.urlretrieve(url, installer)
    run_command([installer, "/quiet", "InstallAllUsers=1", "PrependPath=1"])
    os.remove(installer)

def setup_project(python_exe):
    print(f"\n[Project] Setting up repo using found Python: {python_exe}")
    
    # 1. Clone
    if not os.path.exists(REPO_DIR):
        run_command(["git", "clone", REPO_URL])
    
    os.chdir(REPO_DIR)
    
    # 2. Checkout Branch
    run_command(["git", "fetch", "origin"])
    run_command(["git", "checkout", "Endee"])
    run_command(["git", "pull", "origin", "Endee"])

    # 3. Create Venv
    if not os.path.exists("venv"):
        print("Creating virtual environment...")
        run_command([python_exe, "-m", "venv", "venv"])
    else:
        print("Virtual environment already exists. Skipping creation.")

    # 4. Install Deps
    # Check OS *again* here to determine correct PIP path
    if get_os_type() == "windows":
        venv_pip = os.path.join("venv", "Scripts", "pip.exe")
    else:
        # MacOS and Linux use this path
        venv_pip = os.path.join("venv", "bin", "pip")

    print(f"Installing dependencies using: {venv_pip}")
    run_command([venv_pip, "install", "--upgrade", "pip"])
    run_command([venv_pip, "install", "endee"])
    run_command([venv_pip, "install", "-e", "."])
    
    return venv_pip

if __name__ == "__main__":
    # 0. Check Git
    if shutil.which("git") is None:
        print("Error: Git is not installed.")
        if get_os_type() == "linux":
            run_command("sudo apt-get update && sudo apt-get install -y git", shell=True)
        else:
            sys.exit(1)

    # 1. Check for Existing Python 3.11
    py_path = find_python311()

    if py_path:
        print("\n" + "="*50)
        print(f"FOUND PYTHON 3.11: {py_path}")
        print("Skipping OS installation steps.")
        print("="*50)
    else:
        # Install if missing
        os_type = get_os_type()
        if os_type == "linux":
            install_linux_strategy()
        elif os_type == "macos":
            install_macos_strategy()
        elif os_type == "windows":
            install_windows_strategy()
        
        # Verify installation
        py_path = find_python311()
        if not py_path:
            print("Error: Installation failed or binary not found.")
            sys.exit(1)

    # 2. Setup Project
    setup_project(py_path)

    print("\n" + "="*50)
    print("SETUP SUCCESSFUL!")
    print("="*50)
    
    if get_os_type() == "windows":
        print(f"To start: {os.path.join(os.getcwd(), 'venv', 'Scripts', 'activate')}")
    else:
        print(f"To start: source {os.path.join(os.getcwd(), 'venv', 'bin', 'activate')}")
    print("="*50)





'''
import subprocess
import sys
import os
import shutil

def run_command(command, shell=False):
    """Runs a shell command and raises an exception on failure."""
    print(f"--> Running: {' '.join(command) if isinstance(command, list) else command}")
    try:
        subprocess.check_call(command, shell=shell)
    except subprocess.CalledProcessError as e:
        print(f"Error executing command: {e}")
        sys.exit(1)

def check_and_install_system_deps():
    print("### Checking System Dependencies ###")
    
    # 1. Update Apt
    run_command("sudo apt-get update", shell=True)

    # 2. Check/Install Git
    if shutil.which("git") is None:
        print("Git not found. Installing...")
        run_command("sudo apt-get install -y git", shell=True)
    else:
        print("Git is already installed.")

    # 3. Check/Install Python 3.11
    # We explicitly check for a python3.11 binary
    if shutil.which("python3.11") is None:
        print("Python 3.11 not found. Installing via deadsnakes PPA...")
        run_command("sudo apt-get install -y software-properties-common", shell=True)
        run_command("sudo add-apt-repository -y ppa:deadsnakes/ppa", shell=True)
        run_command("sudo apt-get update", shell=True)
        run_command("sudo apt-get install -y python3.11 python3.11-venv python3.11-dev", shell=True)
    else:
        print("Python 3.11 is already installed.")

def setup_repo():
    repo_url = "https://github.com/EndeeLabs/VectorDBBench.git"
    repo_dir = "VectorDBBench"

    print("\n### Setting up Repository ###")

    # 1. Clone
    if not os.path.exists(repo_dir):
        run_command(["git", "clone", repo_url])
    else:
        print(f"Directory {repo_dir} already exists. Skipping clone.")

    # Change working directory to the repo
    os.chdir(repo_dir)

    # 2. Switch Branch
    print("Switching to 'Endee' branch...")
    run_command(["git", "fetch", "origin"])
    run_command(["git", "checkout", "Endee"])
    
    # Pull latest just in case
    run_command(["git", "pull", "origin", "Endee"])

    return os.getcwd()

def setup_venv_and_install(repo_path):
    print("\n### Setting up Virtual Environment & Installing Packages ###")
    
    venv_dir = os.path.join(repo_path, "venv")
    
    # 1. Create Venv using Python 3.11 specifically
    if not os.path.exists(venv_dir):
        print("Creating virtual environment...")
        run_command(["python3.11", "-m", "venv", "venv"])
    else:
        print("Virtual environment already exists.")

    # Define paths to the venv executables
    venv_python = os.path.join(venv_dir, "bin", "python")
    venv_pip = os.path.join(venv_dir, "bin", "pip")

    # Upgrade pip first
    run_command([venv_python, "-m", "pip", "install", "--upgrade", "pip"])

    # 2. pip install endee
    print("Installing 'endee'...")
    run_command([venv_pip, "install", "endee"])

    # 3. pip install -e .
    print("Installing editable project...")
    run_command([venv_pip, "install", "-e", "."])

    return venv_dir

if __name__ == "__main__":
    # Ensure we are on Linux (quick check)
    if not sys.platform.startswith('linux'):
        print("Warning: This script is optimized for Linux (AWS/GCP instances).")

    # Step 1: System Level Installs (Sudo required)
    check_and_install_system_deps()

    # Step 2: Clone and Switch Branch
    project_path = setup_repo()

    # Step 3 & 4: Create Venv and Install Pip packages
    venv_path = setup_venv_and_install(project_path)

    print("\n" + "="*50)
    print("SETUP COMPLETE!")
    print("="*50)
    print("To start using VectorDBBench, run the following command in your shell:")
    print(f"\n    source {os.path.join(project_path, 'venv/bin/activate')}\n")
    print("="*50)

'''