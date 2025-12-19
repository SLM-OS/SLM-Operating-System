# Setting Up Cygwin as CLion's Terminal

This guide covers configuring CLion to use Cygwin Bash instead of PowerShell or Command Prompt.

---

## Why Cygwin?

Cygwin provides a full POSIX-compatible environment on Windows, offering a more complete Unix-like experience than alternatives like Git Bash.

| Feature | Cygwin | Git Bash | PowerShell |
|---------|--------|----------|------------|
| Unix commands | Full set | Common subset | Aliases only |
| Package manager | Yes (setup.exe) | Limited | No |
| POSIX compatibility | Full | Partial | No |
| `make` included | Installable | Sometimes | No |
| Unix software ports | Thousands | Dozens | N/A |

---

## Cygwin vs Git Bash

### Why choose Cygwin

- **More complete:** Thousands of Unix packages available
- **True POSIX layer:** Better compatibility with Unix scripts and tools
- **Package manager:** Easy to install additional tools (`gcc`, `gdb`, `vim`, etc.)
- **Mature:** Actively developed since 1995

### Trade-offs

- **Larger footprint:** Cygwin DLL and utilities take more disk space
- **Path translation:** Uses `/cygdrive/c/` instead of `C:\` (minor learning curve)
- **Slightly slower:** POSIX emulation layer adds small overhead

For SLM-OS development, Cygwin is an excellent choice since we're cross-compiling for ARM anyway.

---

## Prerequisites

- Cygwin installed (https://cygwin.com/install.html)
- CLion installed

---

## Step 1: Install Required Cygwin Packages

If you haven't already, run the Cygwin installer (`setup-x86_64.exe`) and ensure these packages are installed from the **Devel** category:

| Package | Purpose |
|---------|---------|
| `make` | Build automation |
| `git` | Version control (optional, you may prefer Windows Git) |
| `vim` or `nano` | Text editing in terminal |

The installer lets you search for packages and select them for installation.

---

## Step 2: Locate Cygwin Bash

Cygwin is typically installed at:

```
C:\cygwin64\bin\bash.exe
```

For 32-bit installations:

```
C:\cygwin\bin\bash.exe
```

---

## Step 3: Configure CLion Terminal

1. Open CLion
2. Go to **File → Settings** (or `Ctrl+Alt+S`)
3. Navigate to **Tools → Terminal**
4. Set **Shell path** to:

```
C:\cygwin64\bin\bash.exe --login -i
```

5. Ensure **Start directory** is set to **Project directory**
6. Click **Apply** then **OK**

---

## Step 4: Test the Terminal

1. Open the Terminal tool window (`Alt+F12`)
2. You should see a bash prompt:

```
username@hostname ~
$
```

3. Navigate to the project:

```bash
cd /cygdrive/h/My\ Drive/Capstone/CS-496-SLM-Operating-System
```

4. Test make:

```bash
make --version
```

---

## Step 5: Configure PATH for ARM Toolchain

Add the ARM toolchain to Cygwin's PATH. Edit `~/.bash_profile`:

```bash
nano ~/.bash_profile
```

Add these lines:

```bash
# ARM bare-metal toolchain
export PATH="/cygdrive/c/Program Files/ArmGNUToolchain/13.3.rel1/aarch64-none-elf/bin:$PATH"

# QEMU (if not in Windows PATH)
export PATH="/cygdrive/c/Program Files/qemu:$PATH"
```

Save and reload:

```bash
source ~/.bash_profile
```

Verify:

```bash
aarch64-none-elf-gcc --version
qemu-system-aarch64 --version
```

---

## Cygwin Path Reference

Cygwin translates Windows paths:

| Windows Path | Cygwin Path |
|--------------|-------------|
| `C:\` | `/cygdrive/c/` |
| `D:\` | `/cygdrive/d/` |
| `H:\My Drive\` | `/cygdrive/h/My Drive/` |
| `C:\Users\John` | `/cygdrive/c/Users/John` |
| Cygwin home | `~` or `/home/username` |

The project path in Cygwin:

```
/cygdrive/h/My Drive/Capstone/CS-496-SLM-Operating-System
```

---

## Optional: Customize Bash

### Useful Aliases

Add to `~/.bash_profile`:

```bash
# Project shortcut
alias cdslm='cd "/cygdrive/h/My Drive/Capstone/CS-496-SLM-Operating-System"'

# Make shortcuts
alias mb='make'
alias mr='make run'
alias md='make debug'
alias mc='make clean'
alias mh='make help'

# General shortcuts
alias ll='ls -la'
alias la='ls -A'
alias cls='clear'

# Git shortcuts
alias gs='git status'
alias gd='git diff'
alias gl='git log --oneline -10'
alias ga='git add'
alias gc='git commit'
```

### Better Prompt

Add to `~/.bash_profile`:

```bash
# Show git branch in prompt
parse_git_branch() {
    git branch 2>/dev/null | sed -e '/^[^*]/d' -e 's/* \(.*\)/ (\1)/'
}

PS1='\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[33m\]$(parse_git_branch)\[\033[00m\]\$ '
```

---

## Optional: Auto-Navigate to Project Directory

A cleaner approach than hardcoding the path in `.bash_profile`:

### Step 1: Set Environment Variable in CLion

1. Go to **Settings → Tools → Terminal**
2. Find **Environment variables**
3. Add:
   ```
   PROJECT_DIR=/cygdrive/h/My Drive/Capstone/CS-496-SLM-Operating-System
   ```

### Step 2: Add to .bash_profile

```bash
# Auto-navigate to project if opened from CLion
if [ -d "${PROJECT_DIR}" ]; then
    cd "$PROJECT_DIR"
fi
```

This approach:

- Works only when launched from CLion (where `PROJECT_DIR` is set)
- Doesn't break if opened from standalone Cygwin
- Can be reused across projects by changing the CLion environment variable

---

## Troubleshooting

### "make: command not found"

Install make via Cygwin setup:

1. Run `setup-x86_64.exe` (the Cygwin installer)
2. Click through until you reach package selection
3. Search for `make`
4. Select it for installation
5. Complete the installation

### ARM toolchain not found

Verify the PATH is correct:

```bash
echo $PATH | tr ':' '\n' | grep -i arm
```

Check the actual installation path on your system and update `~/.bash_profile` accordingly.

### "command not found" for Windows programs

Windows programs should work if they're in the Windows PATH. If not, add them to Cygwin's PATH in `~/.bash_profile`:

```bash
export PATH="/cygdrive/c/Program Files/SomeProgram:$PATH"
```

### Slow terminal startup

The `--login` flag loads profile scripts. If too slow, you can use:

```
C:\cygwin64\bin\bash.exe -i
```

But you'll lose custom PATH and aliases from `.bash_profile`.

### Line ending issues with scripts

Cygwin expects Unix line endings (LF). If you see errors like `$'\r': command not found`, convert the file:

```bash
dos2unix filename.sh
```

Install `dos2unix` via Cygwin setup if needed.

### Path with spaces not working

Always quote paths with spaces:

```bash
cd "/cygdrive/h/My Drive/Capstone"
```

Or escape spaces:

```bash
cd /cygdrive/h/My\ Drive/Capstone
```

---

## CLion Terminal Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Alt+F12` | Open/focus terminal |
| `Ctrl+Shift+T` | New terminal tab |
| `Alt+Left/Right` | Switch terminal tabs |
| `Ctrl+W` | Close terminal tab |

---

## Summary

| Setting | Value |
|---------|-------|
| Shell path | `C:\cygwin64\bin\bash.exe --login -i` |
| Start directory | Project directory |
| Required packages | `make` (from Devel category) |

---

*Last updated: December 2025*
