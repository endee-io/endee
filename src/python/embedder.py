import numpy as np

def embed(text: str):
    # Fast dummy embedding (instant startup)
    return np.random.rand(384).astype("float32")