"""
Client-side encryption module (WOW Feature #1).
Encrypts document content before storing in Endee, decrypts on retrieval.
Uses Fernet symmetric encryption (AES-128-CBC).
"""
import base64
import os
from cryptography.fernet import Fernet
from config import ENCRYPTION_KEY


class DocumentEncryptor:
    """Handles client-side encryption/decryption of document content."""

    def __init__(self, key: str = None):
        if key:
            self._key = key.encode() if isinstance(key, str) else key
        elif ENCRYPTION_KEY:
            self._key = ENCRYPTION_KEY.encode() if isinstance(ENCRYPTION_KEY, str) else ENCRYPTION_KEY
        else:
            self._key = Fernet.generate_key()
            print(f"[Encryption] Generated new key: {self._key.decode()}")
            print("[Encryption] Save this key in .env as ENCRYPTION_KEY to persist across sessions.")

        self._fernet = Fernet(self._key)
        self._enabled = True

    @property
    def key(self) -> str:
        return self._key.decode()

    @property
    def enabled(self) -> bool:
        return self._enabled

    @enabled.setter
    def enabled(self, value: bool):
        self._enabled = value

    def encrypt(self, plaintext: str) -> str:
        """Encrypt a string. Returns base64-encoded ciphertext."""
        if not self._enabled:
            return plaintext
        token = self._fernet.encrypt(plaintext.encode("utf-8"))
        return base64.urlsafe_b64encode(token).decode("utf-8")

    def decrypt(self, ciphertext: str) -> str:
        """Decrypt a base64-encoded ciphertext back to plaintext."""
        if not self._enabled:
            return ciphertext
        try:
            token = base64.urlsafe_b64decode(ciphertext.encode("utf-8"))
            return self._fernet.decrypt(token).decode("utf-8")
        except Exception:
            # If decryption fails, return as-is (might be unencrypted)
            return ciphertext

    def encrypt_metadata(self, meta: dict) -> dict:
        """Encrypt sensitive fields in metadata dict."""
        if not self._enabled:
            return meta
        encrypted = {}
        for k, v in meta.items():
            if k in ("text", "content", "chunk_text"):
                encrypted[k] = self.encrypt(str(v))
                encrypted[f"{k}_encrypted"] = True
            else:
                encrypted[k] = v
        return encrypted

    def decrypt_metadata(self, meta: dict) -> dict:
        """Decrypt sensitive fields in metadata dict."""
        if not self._enabled:
            return meta
        decrypted = {}
        for k, v in meta.items():
            if k.endswith("_encrypted"):
                continue
            if meta.get(f"{k}_encrypted", False):
                decrypted[k] = self.decrypt(str(v))
            else:
                decrypted[k] = v
        return decrypted


# Global instance
encryptor = DocumentEncryptor()
