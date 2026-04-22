#!/bin/bash
set -e

echo "Setting up external disk mount..."

echo ""
echo "Available block devices:"
mapfile -t DISKS < <(lsblk -d -o NAME,SIZE,TYPE,MODEL --noheadings | grep -v "^loop")
for i in "${!DISKS[@]}"; do
  echo "  $((i+1))) ${DISKS[$i]}"
done

echo ""
read -rp "Enter the number of the disk to partition and mount: " DISK_NUM

DISK_IDX=$((DISK_NUM - 1))
if [[ -z "${DISKS[$DISK_IDX]}" ]]; then
  echo "Error: Invalid selection." >&2
  exit 1
fi

DISK_NAME=$(echo "${DISKS[$DISK_IDX]}" | awk '{print $1}')
DISK="/dev/${DISK_NAME}"

if [[ "$DISK_NAME" == nvme* ]]; then
  PARTITION="${DISK}p1"
else
  PARTITION="${DISK}1"
fi

echo ""
echo "Selected disk : $DISK"
echo "Partition     : $PARTITION"
read -rp "Proceed? This will ERASE all data on $DISK. [y/N] " CONFIRM
if [[ "$CONFIRM" != "y" && "$CONFIRM" != "Y" ]]; then
  echo "Aborted."
  exit 1
fi

echo "Creating mount directory 'mnt'..."
mkdir -p $HOME/mnt

echo "Updating packages and installing required tools (fdisk tmux htop nvme-cli)..."
sudo apt update
sudo apt install -y fdisk tmux htop nvme-cli
echo "Packages installed."

echo ""
echo "1. Partitioning the disk $DISK..."
echo "   Running: sudo fdisk $DISK (creating one primary partition using all defaults)"
printf 'n\np\n\n\n\nw\n' | sudo fdisk "$DISK"
echo "   Partitioning done."

echo ""
echo "2. Formatting and mounting the partition..."
echo "   Running: sudo mkfs.ext4 $PARTITION"
sudo mkfs.ext4 "$PARTITION"
echo "   Running: sudo mount $PARTITION mnt"
sudo mount "$PARTITION" mnt
echo "   Running: sudo chown \$USER mnt"
sudo chown $USER mnt
echo "   Verifying with lsblk..."
lsblk
echo ""
echo "3. Configure Docker to wait for local filesystem..."
echo "   Running: sudo mkdir -p /etc/systemd/system/docker.service.d"
sudo mkdir -p /etc/systemd/system/docker.service.d
echo "   Running: writing override.conf"
echo -e '[Unit]\nAfter=local-fs.target\nRequires=local-fs.target' | sudo tee /etc/systemd/system/docker.service.d/override.conf
echo "   Running: sudo systemctl daemon-reload"
sudo systemctl daemon-reload
echo "   Done. Verifying Docker service dependencies..."
systemctl show docker.service -p After -p Requires

echo ""
echo "4. Add system binary paths to PATH..."
echo "   Running: adding PATH export to ~/.bashrc"
grep -qxF 'export PATH="$PATH:/usr/sbin:/sbin"' ~/.bashrc || echo 'export PATH="$PATH:/usr/sbin:/sbin"' >> ~/.bashrc
echo "   Running: source ~/.bashrc"
source ~/.bashrc
echo "   Done."

echo ""
echo "5. Run generate_fstab.sh to make mount permanent"
