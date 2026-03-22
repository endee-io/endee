# Endee evaluation: GitHub star, fork, and project base

These steps must be done **on your GitHub account** (the evaluator cannot do them for you).

## 1. Star the official repository

1. Open **[https://github.com/endee-io/endee](https://github.com/endee-io/endee)**.
2. Click **Star** (top right).

## 2. Fork to your account

1. On the same page, click **Fork**.
2. Choose your personal account (or org) and create the fork.
3. Your fork will be at: `https://github.com/<YOUR_USERNAME>/<your-fork-name>` (for example [CODEMASTERSTACK/Endee---Projection](https://github.com/CODEMASTERSTACK/Endee---Projection), clone URL: `https://github.com/CODEMASTERSTACK/Endee---Projection.git`).

## 3. Use the fork as the base for this project

The evaluation expects your work to live **on top of the forked repo**, not only as a loose folder on disk.

### Recommended layout (clone fork, then add this app inside it)

**Example using your fork** [CODEMASTERSTACK/Endee---Projection](https://github.com/CODEMASTERSTACK/Endee---Projection):

```powershell
cd $env:USERPROFILE\Desktop
git clone https://github.com/CODEMASTERSTACK/Endee---Projection.git
cd Endee---Projection

# Dedicated folder for your submission (name is yours; keep it clear)
mkdir applications
```

Copy everything from your local **Private HR Assistant** project into the fork, for example:

```powershell
# Adjust the source path if your project lives elsewhere
Copy-Item -Recurse "C:\Users\Krish\Desktop\New folder\*" ".\applications\private-hr-assistant\"
```

Then commit and push **from inside the fork** (this upstream uses **`master`** as the default branch):

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection
git checkout -b project/private-hr-assistant
git add applications/private-hr-assistant
git commit -m "Add privacy-first HR assistant (Flutter + FastAPI + Endee)"
git push -u origin project/private-hr-assistant
```

Generic template (any fork name):

```powershell
cd $env:USERPROFILE\Desktop
git clone https://github.com/YOUR_USERNAME/YOUR_FORK_NAME.git
cd YOUR_FORK_NAME
mkdir applications
Copy-Item -Recurse "C:\Users\Krish\Desktop\New folder\*" ".\applications\private-hr-assistant\"
git checkout -b project/private-hr-assistant
git add applications/private-hr-assistant
git commit -m "Add privacy-first HR assistant (Flutter + FastAPI + Endee)"
git push -u origin project/private-hr-assistant
```

Optional: open a Pull Request from that branch to your own `main`, or merge locally—what matters is that **your fork** contains both upstream Endee history and your application tree.

### Keep upstream Endee in sync (optional)

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection
git remote add upstream https://github.com/endee-io/endee.git
git fetch upstream
# Later: merge or rebase upstream/master into your branch as needed
```

## 4. What stays true in this project

- **Endee server**: still run via Docker / docs ([Quick Start](https://docs.endee.io/quick-start)); your fork holds **source** for the database product if you need it.
- **Python SDK**: continue using `pip install endee` / `requirements.txt` as documented in `README.md`; you do not need to vendor the SDK into your app folder unless the evaluation explicitly asks for it.

## 5. If you use GitHub CLI (`gh`)

After `gh auth login`:

```powershell
gh repo fork endee-io/endee --clone=true
# Clones YOUR fork; then add applications/private-hr-assistant as above
```

Starring is still one click on the website (or `gh repo star endee-io/endee` if available in your `gh` version).

## 6. Push rejected: large files (e.g. `torch_cpu.dll` in `.venv`)

**Do not commit Python virtual environments.** Packages like PyTorch include DLLs over GitHub’s **100 MB** per-file limit.

In your **fork clone** (e.g. `Endee---Projection`):

```powershell
cd $env:USERPROFILE\Desktop\Endee---Projection

# Stop tracking the venv (keeps folder on disk)
git rm -r --cached applications/private-hr-assistant/backend/.venv

# Ensure gitignore ignores any venv under applications/ (copy updated .gitignore from this project or append):
#   .venv/
#   **/.venv/
```

Merge the updated `.gitignore` from the `private-hr-assistant` project into the fork root or into `applications/private-hr-assistant/`, then:

```powershell
git add .gitignore applications/private-hr-assistant/.gitignore
git commit -m "Remove venv from git; ignore .venv directories"
git push -u origin project/private-hr-assistant
```

If GitHub **still** rejects the push, the large blob is in an **older commit** on that branch. Easiest fix when the bad commit is the **latest**:

```powershell
git reset --soft HEAD~1
git rm -r --cached applications/private-hr-assistant/backend/.venv
# fix .gitignore, then
git add .
git commit -m "Add private HR assistant without venv"
git push -f origin project/private-hr-assistant
```

If history is deeper, use [git-filter-repo](https://github.com/newren/git-filter-repo) to strip `applications/private-hr-assistant/backend/.venv` from all commits on the branch.

After a successful push, recreate the venv on each machine with:

`cd applications/private-hr-assistant/backend` → `python -m venv .venv` → `.\.venv\Scripts\pip install -r requirements.txt`.

Still blocked? Follow **[GIT_FIX_LARGE_FILES.md](GIT_FIX_LARGE_FILES.md)** (list largest blobs, then `git filter-repo` or a clean branch).
