#!/bin/bash
set -e

# Check if domain is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <domain> [www-domain]"
  echo "Example: $0 example.com www.example.com"
  exit 1
fi

DOMAIN=$1
WWW_DOMAIN=${2:-www.$DOMAIN}

echo "Setting up SSL certificate for $DOMAIN and $WWW_DOMAIN..."

# Install Certbot and Nginx plugin
sudo apt-get update
sudo apt-get install -y certbot python3-certbot-nginx

# Issue SSL certificate for your domain
sudo certbot --nginx -d $DOMAIN -d $WWW_DOMAIN

# Verify certificate auto-renewal is configured
sudo certbot renew --dry-run

# Check certificate status
sudo certbot certificates

echo "SSL certificate setup complete!"
echo "Certificate location: /etc/letsencrypt/live/$DOMAIN/"
