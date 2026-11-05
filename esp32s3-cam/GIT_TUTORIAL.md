# Git Tutorial For This Project

> Historical tutorial retained from the original camera project. Names such as
> `master` and `backup/platformio-original` below are not claims about branches
> available in this split repository. Follow [the branch README](../README.md)
> for the current component layout and merge precautions.

This tutorial is written for the current repository and uses the same operations that
were already performed here.

## 1. What Git Solves

Git is a version history tool. You can treat it like a time machine for your code:

- save the current state
- compare changes
- return to an older version
- create a safe branch before trying a new idea

## 2. What Was Already Done Here

The repository was initialized and the original PlatformIO version was backed up first.

Commands used:

```powershell
git init
git add .
git commit -m "backup: platformio project before idf migration"
git branch backup/platformio-original
```

Meaning:

- `git init`: turns the folder into a Git repository
- `git add .`: puts current files into the staging area
- `git commit -m "..."`
  : creates a snapshot you can return to later
- `git branch backup/platformio-original`
  : gives the original version a fixed branch name

## 3. The Three Git Areas

Understanding these three areas makes Git much easier:

- working directory: the files you are editing right now
- staging area: the files you want to include in the next snapshot
- repository history: the snapshots already saved by `git commit`

## 4. Daily Workflow

Use this as your normal habit:

```powershell
git status
git add .
git commit -m "describe what changed"
```

A simple example:

```powershell
git status
git add main/main.c
git commit -m "feat: improve udp camera sender"
```

## 5. Commands You Will Use Most Often

### Check status

```powershell
git status
```

This tells you:

- which files changed
- which files are staged
- which branch you are on

### See history

```powershell
git log --oneline --decorate --graph --all
```

This is the fastest way to understand your version history.

### See file differences

```powershell
git diff
git diff --staged
```

- `git diff`: unstaged changes
- `git diff --staged`: staged changes

### Create a branch before a risky change

```powershell
git switch -c feature/add-web-preview
```

This creates and switches to a new branch.

### Switch back to an existing branch

```powershell
git switch master
git switch backup/platformio-original
```

## 6. How To Return To The Original PlatformIO Backup

If you want to see the original project before the ESP-IDF migration:

```powershell
git switch backup/platformio-original
```

If you want to return to the current development branch later:

```powershell
git switch master
```

## 7. How To Undo Things Safely

### Unstage a file

```powershell
git restore --staged main/main.c
```

This removes the file from the staging area, but keeps your code changes.

### Discard local changes in one file

```powershell
git restore main/main.c
```

This resets the file back to the last commit. Use carefully.

### Go back by creating a new reversing commit

```powershell
git revert <commit-id>
```

This is safer than rewriting history.

## 8. Good Commit Message Examples

Recommended styles:

- `feat: add udp reconnect logic`
- `fix: handle camera init failure`
- `refactor: move wifi setup into helper`
- `docs: add git tutorial for this project`

Try to make each commit answer one question:

"What changed?"

## 9. Very Practical Habit For You

When you are about to change hardware pins, Wi-Fi code, camera settings, or flash logic:

1. run `git status`
2. run `git add .`
3. run `git commit -m "backup: before changing camera pins"`
4. then start modifying code

If the experiment fails, you can always return to the saved point.

## 10. Recommended Recovery Commands

When you feel confused, use these first:

```powershell
git status
git log --oneline --decorate --graph --all
git diff
```

These three commands solve most beginner Git confusion.

## 11. A Safe Beginner Rule

If you are not sure what a Git command will do, do not run commands like these casually:

- `git reset --hard`
- `git clean -fd`
- forced push operations

These can remove work.

## 12. Your Current Useful Branches

Right now this repository has these important meanings:

- `master`: current working branch
- `backup/platformio-original`: the original PlatformIO backup

## 13. Suggested Next Habit

After every small feature or successful hardware test, create one commit.

Small commits are easier to understand, compare, and roll back than one huge commit.
