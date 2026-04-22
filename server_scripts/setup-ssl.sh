#!/bin/bash
set -e

# Check if domain is provided
if [ -z "$1" ]; then
  echo "Usage: $0 <domain>"
  echo "Example: $0 example.com"
  exit 1
fi

DOMAIN=$1
WWW_DOMAIN=www.$DOMAIN

echo "Setting up SSL certificate for $DOMAIN and $WWW_DOMAIN..."

# Install Certbot and Nginx plugin
sudo apt-get update
sudo apt-get install -y certbot python3-certbot-nginx

# Create basic HTTP Nginx config first — certbot --nginx needs an existing
# server_name block to find and modify
echo "Creating Nginx config /etc/nginx/conf.d/${DOMAIN}.conf..."
sudo tee /etc/nginx/conf.d/${DOMAIN}.conf > /dev/null <<EOF
server {
    listen 80;
    listen [::]:80;
    server_name ${DOMAIN};

    root /var/www/html;
    index index.html index.htm;

    location / {
        try_files \$uri \$uri/ =404;
    }

    # Proxy example (uncomment and update to use)
    # location /api/ {
    #     proxy_pass http://127.0.0.1:8080/;
    #     proxy_http_version 1.1;
    #     proxy_set_header Host \$host;
    #     proxy_set_header X-Real-IP \$remote_addr;
    #     proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    #     proxy_set_header X-Forwarded-Proto \$scheme;
    # }
}
EOF

sudo nginx -t
sudo systemctl reload nginx

# Issue SSL certificate — certbot modifies the config above to add SSL and
# HTTP→HTTPS redirect automatically
sudo certbot --nginx -d $DOMAIN

# Verify certificate auto-renewal is configured
sudo certbot renew --dry-run

# Check certificate status
sudo certbot certificates

echo "SSL certificate setup complete!"
echo "Certificate location: /etc/letsencrypt/live/$DOMAIN/"
echo "Config created at /etc/nginx/conf.d/${DOMAIN}.conf"
