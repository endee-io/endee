# Nexus Deployment Guide

## Overview

This guide covers deploying Nexus to production environments. We'll deploy:
- **Frontend** → Vercel (recommended) or Netlify
- **Backend** → Render, Railway, or AWS
- **Endee** → Docker container on cloud VM

---

## Prerequisites

- [ ] GitHub repository with Nexus code
- [ ] Accounts on deployment platforms
- [ ] Domain name (optional but recommended)
- [ ] SSL certificate (auto-provided by most platforms)

---

## Architecture Overview

```
Internet → CDN (Vercel) → Next.js Frontend
                ↓
         Load Balancer → FastAPI Backend
                ↓
         Docker VM → Endee Vector DB
```

---

## Part 1: Deploy Endee (Vector Database)

### Option A: DigitalOcean Droplet

**1. Create Droplet:**
```bash
# Specs recommended:
CPU: 2 vCPUs
RAM: 4 GB
Storage: 80 GB SSD
OS: Ubuntu 22.04
```

**2. SSH into droplet:**
```bash
ssh root@your-droplet-ip
```

**3. Install Docker:**
```bash
curl -fsSL https://get.docker.com -o get-docker.sh
sh get-docker.sh
```

**4. Clone repository:**
```bash
git clone <your-repo-url>
cd endee-network-map
```

**5. Build Endee Docker image:**
```bash
docker build -t endee -f infra/Dockerfile .
```

**6. Run Endee container:**
```bash
docker run -d \
  --name endee \
  -p 3001:3001 \
  -v /data/endee:/data \
  --restart unless-stopped \
  endee
```

**7. Verify it's running:**
```bash
curl http://localhost:3001/health
```

**8. Configure firewall:**
```bash
ufw allow 3001/tcp
ufw enable
```

**Note your droplet IP:** `http://YOUR_DROPLET_IP:3001`

### Option B: AWS EC2

Similar to DigitalOcean, but:
- Use EC2 t3.medium instance
- Configure Security Group to allow port 3001
- Use Elastic IP for static address

---

## Part 2: Deploy Backend (FastAPI)

### Option A: Render

**1. Sign up at [render.com](https://render.com)**

**2. Create New Web Service:**
- Connect GitHub repository
- Select `nexus-backend` directory (or configure Root Directory)

**3. Configure Build:**
```
Build Command: pip install -r requirements.txt
Start Command: uvicorn main:app --host 0.0.0.0 --port $PORT
```

**4. Environment Variables:**
```
ENDEE_URL=http://YOUR_DROPLET_IP:3001
ENDEE_INDEX=nexus_knowledge
EMBEDDING_MODEL=all-MiniLM-L6-v2
```

**5. Deploy:**
- Click "Create Web Service"
- Wait for deployment (5-10 minutes)
- Note your backend URL: `https://your-app.onrender.com`

### Option B: Railway

**1. Install Railway CLI:**
```bash
npm install -g railway
```

**2. Login and init:**
```bash
cd nexus-backend
railway login
railway init
```

**3. Add environment variables:**
```bash
railway variables set ENDEE_URL=http://YOUR_DROPLET_IP:3001
railway variables set ENDEE_INDEX=nexus_knowledge
```

**4. Deploy:**
```bash
railway up
```

**5. Get domain:**
```bash
railway domain
```

### Option C: AWS Elastic Beanstalk

**1. Install EB CLI:**
```bash
pip install awsebcli
```

**2. Initialize:**
```bash
cd nexus-backend
eb init -p python-3.10 nexus-backend
```

**3. Create environment:**
```bash
eb create nexus-production
```

**4. Set environment variables:**
```bash
eb setenv ENDEE_URL=http://YOUR_DROPLET_IP:3001
eb setenv ENDEE_INDEX=nexus_knowledge
```

**5. Deploy:**
```bash
eb deploy
```

---

## Part 3: Deploy Frontend (Next.js)

### Option A: Vercel (Recommended)

**1. Sign up at [vercel.com](https://vercel.com)**

**2. Import repository:**
- Click "New Project"
- Import your GitHub repository
- Select `nexus-frontend` as root directory

**3. Configure:**
```
Framework Preset: Next.js
Build Command: npm run build
Output Directory: .next
Install Command: npm install
```

**4. Environment Variables:**
```
NEXT_PUBLIC_API_URL=https://your-backend.onrender.com
```

**5. Deploy:**
- Click "Deploy"
- Wait 2-3 minutes
- Your site will be live at: `https://your-project.vercel.app`

**6. Custom domain (optional):**
- Go to Settings → Domains
- Add your domain (e.g., `nexus.yourdomain.com`)
- Configure DNS as instructed

### Option B: Netlify

**1. Sign up at [netlify.com](https://netlify.com)**

**2. New site from Git:**
- Connect GitHub
- Select repository

**3. Build settings:**
```
Base directory: nexus-frontend
Build command: npm run build
Publish directory: .next
```

**4. Environment variables:**
```
NEXT_PUBLIC_API_URL=https://your-backend.onrender.com
```

**5. Deploy:**
- Click "Deploy site"

---

## Post-Deployment Configuration

### 1. Initialize Endee Index

```bash
curl -X POST https://your-backend.onrender.com/api/initialize
```

### 2. Test Complete Flow

**Upload a document:**
```bash
curl -X POST https://your-backend.onrender.com/api/documents/upload \
  -F "file=@test.pdf"
```

**Check graph:**
```bash
curl https://your-backend.onrender.com/api/graph
```

**Visit frontend:**
```
https://your-project.vercel.app
```

### 3. Enable CORS for Production

Update `nexus-backend/main.py`:

```python
app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "https://your-project.vercel.app",
        "https://nexus.yourdomain.com"  # If using custom domain
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
```

Redeploy backend after updating.

---

## Monitoring & Maintenance

### Health Checks

**Backend health endpoint:**
```bash
curl https://your-backend.onrender.com/health
```

**Endee health endpoint:**
```bash
curl http://YOUR_DROPLET_IP:3001/health
```

### Logging

**Backend logs (Render):**
- Dashboard → Logs tab

**Endee logs:**
```bash
ssh root@your-droplet-ip
docker logs endee
```

### Backup Strategy

**Endee data backup:**
```bash
# On droplet
tar -czf endee-backup-$(date +%Y%m%d).tar.gz /data/endee

# Download locally
scp root@your-droplet-ip:endee-backup-*.tar.gz ./backups/
```

**Automated backup script:**
```bash
#!/bin/bash
# /root/backup-endee.sh

BACKUP_DIR="/backups"
mkdir -p $BACKUP_DIR

tar -czf $BACKUP_DIR/endee-$(date +%Y%m%d-%H%M%S).tar.gz /data/endee

# Keep only last 7 backups
ls -t $BACKUP_DIR/endee-*.tar.gz | tail -n +8 | xargs rm -f
```

**Add to crontab:**
```bash
crontab -e
# Add: 0 2 * * * /root/backup-endee.sh
```

---

## Security Best Practices

### 1. Environment Variables

**Never commit:**
- API keys
- Database credentials
- Auth tokens

**Use platform-provided secrets management**

### 2. HTTPS Everywhere

- Vercel/Netlify provide free SSL
- For Endee, consider using Cloudflare or nginx reverse proxy

### 3. Rate Limiting

Add to backend:
```python
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.util import get_remote_address

limiter = Limiter(key_func=get_remote_address)
app.state.limiter = limiter

@app.post("/api/documents/upload")
@limiter.limit("10/hour")
async def upload_document(...):
    pass
```

### 4. Authentication (Optional)

Add JWT authentication for production:
```python
from fastapi.security import HTTPBearer

security = HTTPBearer()

@app.post("/api/documents/upload")
async def upload_document(
    credentials: HTTPAuthorizationCredentials = Security(security)
):
    # Validate token
    token = credentials.credentials
    # ...
```

---

## Performance Optimization

### Backend Optimization

**1. Use Gunicorn with workers:**
```bash
# Start command on Render/Railway:
gunicorn main:app --workers 4 --worker-class uvicorn.workers.UvicornWorker --bind 0.0.0.0:$PORT
```

**2. Add caching:**
```python
from functools import lru_cache

@lru_cache(maxsize=128)
async def get_cached_graph(threshold: float):
    return await build_graph(threshold)
```

### Frontend Optimization

**1. Enable Next.js ISR:**
```tsx
export const revalidate = 60 // Revalidate every 60 seconds
```

**2. Image optimization:**
```tsx
import Image from 'next/image'

<Image src="/logo.png" width={100} height={100} alt="Logo" />
```

### Endee Optimization

**1. Tune HNSW parameters:**
- Increase `M` for better recall
- Increase `ef_construction` for better quality

**2. Use appropriate quantization:**
- `int8` for memory efficiency
- `float32` for maximum accuracy

---

## Scaling Strategy

### Horizontal Scaling

**Backend:**
- Add more instances on Render/Railway
- Use load balancer

**Endee:**
- Implement sharding for large datasets
- Use read replicas for query performance

### Vertical Scaling

**Endee Droplet:**
- Upgrade to larger instance
- Add more CPU/RAM as needed

---

## Cost Estimation

### Monthly Costs (approximate)

**Minimal Setup:**
- Vercel (Frontend): $0 (Hobby plan)
- Render (Backend): $7 (Starter plan)
- DigitalOcean (Endee): $12 (Basic droplet)
- **Total: ~$20/month**

**Production Setup:**
- Vercel (Frontend): $20 (Pro plan)
- Render (Backend): $50 (Standard plan)
- DigitalOcean (Endee): $48 (4GB RAM)
- **Total: ~$120/month**

---

## Rollback Strategy

### Backend Rollback (Render)

```bash
# Via dashboard
- Settings → Rollback → Select previous deployment
```

### Frontend Rollback (Vercel)

```bash
# Via CLI
vercel rollback
```

### Endee Rollback

```bash
# Stop current container
docker stop endee

# Remove container
docker rm endee

# Restore from backup
tar -xzf endee-backup-20240210.tar.gz -C /

# Restart with previous image
docker run -d --name endee ...
```

---

## Domain Configuration

### Custom Domain Setup

**1. Purchase domain** (Namecheap, Google Domains, etc.)

**2. Configure DNS:**

```
Type   Name      Value                          TTL
A      @         YOUR_BACKEND_IP                3600
CNAME  www       your-project.vercel.app        3600
CNAME  api       your-backend.onrender.com      3600
```

**3. SSL certificates:**
- Automatically provided by Vercel/Render

**4. Update CORS in backend:**
```python
allow_origins=[
    "https://yourdomain.com",
    "https://www.yourdomain.com"
]
```

---

## Troubleshooting

### Issue: Frontend can't connect to backend

**Check:**
1. Backend is running: `curl https://your-backend.onrender.com/health`
2. Environment variable is correct
3. CORS is properly configured

### Issue: Endee connection timeout

**Check:**
1. Endee docker container is running: `docker ps`
2. Port 3001 is open in firewall
3. Backend can reach Endee IP

### Issue: Slow embedding generation

**Solution:**
- Use GPU-enabled backend instance
- Implement caching for common queries
- Consider using a dedicated embedding API service

---

## Monitoring Tools

### Recommended Services

**Application Monitoring:**
- [DataDog](https://www.datadoghq.com/)
- [New Relic](https://newrelic.com/)
- [Sentry](https://sentry.io/) (error tracking)

**Uptime Monitoring:**
- [UptimeRobot](https://uptimerobot.com/)
- [Pingdom](https://www.pingdom.com/)

**Analytics:**
- [Google Analytics](https://analytics.google.com/)
- [Plausible](https://plausible.io/) (privacy-focused)

---

## Conclusion

You now have:
- ✅ Production-ready deployment
- ✅ Scalable architecture
- ✅ Monitoring setup
- ✅ Backup strategy

**Next steps:**
- Set up monitoring alerts
- Configure automated backups
- Implement authentication
- Add analytics

**Need help?** Open an issue on GitHub or consult the main documentation.

---

**Congratulations on deploying Nexus! 🚀**
