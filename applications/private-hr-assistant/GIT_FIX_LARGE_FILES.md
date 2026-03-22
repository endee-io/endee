# GitHub still rejects the push (>100 MB)

GitHub blocks **any single file over 100 MB**. Removing `.venv` in a **new commit** is not enough if older commits still contain `torch_cpu.dll` or other large blobs — those stay in **history** until you strip them.

## 1) See what Git is still storing (largest blobs)

Run inside your **fork clone** (e.g. `Endee---Projection`), in PowerShell:

```powershell
git rev-list --objects --all |
  git cat-file --batch-check='%(objecttype) %(objectname) %(objectsize) %(rest)' |
  Where-Object { $_ -like 'blob *' } |
  ForEach-Object {
    $p = $_ -split ' ', 4
    [pscustomobject]@{ SizeMB = [math]::Round([int64]$p[2]/1MB, 2); Path = $p[3] }
  } |
  Sort-Object SizeMB -Descending |
  Select-Object -First 25
```

Anything under `.venv`, `.gradle`, `site-packages`, or `.cache` should not be in Git.

## 2) Fix: remove paths from **all history** (recommended)

Install [git-filter-repo](https://github.com/newren/git-filter-repo) (one-time):

```powershell
pip install git-filter-repo
```

In your fork clone:

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection

# Remove every .venv directory from the entire history (adjust if your app path differs)
git filter-repo --force --invert-paths --path-glob '**/.venv/**'
```

If your app only lives under `applications/private-hr-assistant/`:

```powershell
git filter-repo --force --invert-paths --path applications/private-hr-assistant/backend/.venv
```

Also strip common huge paths if they were committed:

```powershell
git filter-repo --force --invert-paths `
  --path-glob '**/.gradle/**' `
  --path-glob '**/android/**/build/**' `
  --path-glob '**/.dart_tool/**'
```

`git filter-repo` **removes the `origin` remote**. Add it back and force-push:

```powershell
git remote add origin https://github.com/CODEMASTERSTACK/Endee---Projection.git
git push -f origin project/private-hr-assistant
# If you also changed master:  git push -f --all origin
```

## 3) Easiest escape hatch: one clean commit from `master`

If history is messy, start over on a **new branch** with **no** `.venv` / Gradle caches on disk:

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection
git fetch origin
git checkout -b project/private-hr-assistant-clean origin/master

# Copy your app again from "New folder", but EXCLUDE venv and build dirs:
# Manually delete before copy:
#   - applications\private-hr-assistant\backend\.venv
#   - frontend\hr_assistant\.dart_tool, build, android\.gradle
# Or use robocopy /XD .venv .gradle .dart_tool build

git add applications/private-hr-assistant
git commit -m "Add private HR assistant (no venv, no build artifacts)"
git push -u origin project/private-hr-assistant-clean
```

Use the updated `.gitignore` from this repo so `.venv` is never added again.

## 4) After a successful push

On each machine:

```powershell
cd applications\private-hr-assistant\backend
python -m venv .venv
.\.venv\Scripts\pip install -r requirements.txt
```

## 5) Why the size **increased** (very common)

- **Deleting a file in a new commit does not remove old blobs.** Every commit that ever added `.venv` or a huge DLL still stores those bytes until history is rewritten or discarded.
- **Each “fix” commit can add more data** if you run `git add .` while `.venv` or build folders still exist on disk (or `.gitignore` was missing in that clone).
- **`git filter-repo` without cleanup** can leave old unreachable objects until you run garbage collection (below).
- **Disk size vs Git size:** Deleting `backend\.venv` in Explorer shrinks your folder, but **`.git`** can still be gigabytes until you prune history.

**Check how big Git thinks the repo is:**

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection
git count-objects -vH
```

## 6) After `git filter-repo`: actually shrink `.git`

```powershell
git reflog expire --expire=now --all
git gc --prune=now --aggressive
git count-objects -vH
```

## 7) Reliable reset: **throw away the bad branch**, one clean commit from `master`

This avoids fighting old commits. You keep the full Endee fork history on `master`; only your **submission branch** is replaced.

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection
git fetch origin
git checkout -B project/private-hr-assistant origin/master

# Remove any old copy of the app (if present)
Remove-Item -Recurse -Force applications\private-hr-assistant -ErrorAction SilentlyContinue

# Copy from your laptop project, EXCLUDING huge dirs (adjust source path)
$src = "C:\Users\Krish\Desktop\New folder"
$dst = "applications\private-hr-assistant"
New-Item -ItemType Directory -Force -Path applications | Out-Null
robocopy $src $dst /E /XD .venv venv .gradle .dart_tool build "android\.gradle" .git /NFL /NDL /NJH /NJS
Copy-Item "C:\Users\Krish\Desktop\New folder\.gitignore" "$dst\.gitignore" -Force

git add applications/private-hr-assistant
git status   # confirm NO .venv / site-packages / .gradle paths listed
git commit -m "Add private HR assistant (source only; no venv or build outputs)"
git push -f origin project/private-hr-assistant
```

On GitHub you can **delete** the old oversized branch (`project/private-hr-assistant` before rewrite) if it still exists, to avoid confusion.

## 8) If **even `master`** on your fork contains large files you pushed

You must run `git filter-repo` on the **whole repository** (or delete the fork and fork again, then push only a clean branch). Back up first.

---

**Rule of thumb:** Never run `git add .` until `git status` and Explorer show **no** `backend\.venv`, no Flutter `build/`, no `android\.gradle`, and your `.gitignore` is in place.
