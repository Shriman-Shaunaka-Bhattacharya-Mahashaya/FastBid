#!/bin/bash
set -euo pipefail

. /etc/os-release
if [[ "$ID" != "ubuntu" ]]; then
    echo "This bootstrap supports Ubuntu only." >&2
    exit 1
fi

echo "====================================="
echo " FastBid Oracle VM Setup Script"
echo "====================================="

# 1. Update and install prerequisites
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg ufw

# 2. Install Docker
if ! command -v docker &> /dev/null; then
    echo "Installing Docker..."
    sudo install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    sudo chmod a+r /etc/apt/keyrings/docker.gpg
    echo "deb [arch="$(dpkg --print-architecture)" signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu "$(. /etc/os-release && echo "$VERSION_CODENAME")" stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
    sudo apt-get update
    sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
fi

# 3. Configure Firewall (UFW & Oracle Cloud iptables)
echo "Configuring Firewall..."
sudo ufw allow 22/tcp       # SSH
sudo ufw allow 80/tcp       # HTTP (SSL challenges / ACME)
sudo ufw allow 443/tcp      # HTTPS / WSS Reverse Proxy
sudo ufw --force enable

# Oracle Cloud Ubuntu images ship with rigid default iptables that intercept
# traffic before UFW rules execute. Explicitly insert ACCEPT rules:
echo "Punching holes through Oracle Cloud OS iptables..."
sudo iptables -I INPUT 1 -p tcp --dport 22 -j ACCEPT 2>/dev/null || true
sudo iptables -I INPUT 2 -p tcp --dport 80 -j ACCEPT 2>/dev/null || true
sudo iptables -I INPUT 3 -p tcp --dport 443 -j ACCEPT 2>/dev/null || true
if command -v netfilter-persistent &> /dev/null; then
    sudo netfilter-persistent save
fi

# 4. OS Tuning for Low Latency
echo "Applying sysctl optimizations..."
cat <<EOF | sudo tee /etc/sysctl.d/99-fastbid.conf
# Prefer RAM (does not disable swap)
vm.swappiness = 0

# Network stack optimizations for low latency
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_fastopen = 3
EOF
sudo sysctl -p /etc/sysctl.d/99-fastbid.conf

# To isolate cores, we would normally edit GRUB:
# GRUB_CMDLINE_LINUX_DEFAULT="... isolcpus=1 rcu_nocbs=1 nohz_full=1"
# update-grub
# For this script, we'll just log it.
echo ""
echo "NOTE: For true core isolation, please add 'isolcpus=1 rcu_nocbs=1 nohz_full=1' to your GRUB cmdline and reboot."
echo ""
echo "Setup complete. From server/, run docker compose up -d --build."
