from __future__ import annotations

from datetime import datetime, timedelta, timezone

import jwt
from fastapi import Depends, HTTPException
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from app.config import get_settings

bearer_scheme = HTTPBearer(auto_error=False)


def create_access_token(sub: str = "hr_user") -> str:
    s = get_settings()
    now = datetime.now(timezone.utc)
    payload = {
        "sub": sub,
        "exp": now + timedelta(minutes=s.jwt_expire_minutes),
        "iat": now,
    }
    return jwt.encode(payload, s.jwt_secret, algorithm=s.jwt_algorithm)


def verify_bearer_or_jwt(credentials: HTTPAuthorizationCredentials | None = Depends(bearer_scheme)) -> str:
    if credentials is None:
        raise HTTPException(status_code=401, detail="Missing Authorization header")
    token = credentials.credentials
    s = get_settings()
    if token == s.api_bearer_token:
        return "demo_user"
    try:
        payload = jwt.decode(token, s.jwt_secret, algorithms=[s.jwt_algorithm])
        sub = payload.get("sub")
        if not sub:
            raise HTTPException(status_code=401, detail="Invalid token payload")
        return str(sub)
    except jwt.PyJWTError:
        raise HTTPException(status_code=401, detail="Invalid or expired token") from None
