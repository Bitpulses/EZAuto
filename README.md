# EZAuto

**EZ** = English + Chinese (**E**nglish, **Z**hongwen) | **Auto** = Automatic

> Stop fighting with Windows IME. Let EZAuto handle it.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Windows](https://img.shields.io/badge/Platform-Windows-0078d4.svg)](https://www.microsoft.com/windows)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()

---

## 📌 The Problem: Windows IME Switcher is Broken

You're working in **VS Code**, typing English — everything is fine.

Then you press `Alt+Tab` to switch to **WeChat** to reply to a message.

**But Windows does something maddening:**

- ❌ Your input method **stays in English** — you can't type Chinese
- ❌ Or worse — Windows **randomly switches back to Chinese** when you return to VS Code
- ❌ Or the input method **resets to Chinese** for no reason at all

Windows has a "smart" feature that tries to remember input methods per window. **But it doesn't work reliably.**

**You waste seconds every time you switch windows. Multiple times per minute. All day long.**

This has been a **known Windows issue for over a decade**. Microsoft hasn't fixed it.

---

## ✨ The Solution: EZAuto

![img](assets/ezauto_screen.gif)

**EZAuto completely bypasses Windows' broken IME management.**

EZAuto ensures the **right input method** is always active — based on **which window** you're using.

**No manual switching. No random resets. No frustration.**

---

## 🚀 Quick Start

### 1. Download

Download `EZAuto_static.exe` from [Releases](https://github.com/Bitpulses/EZAuto/releases/)

### 2. Run

Double-click `EZAuto_static.exe`

### 3. It just works

Switch between windows. EZAuto changes input method automatically.

**That's it. No config needed.**

---

## ⚙️ Configuration (Optional)

Create `ezauto.json` in the same folder:

```json
{
    "default_mode": "english",
    "switch_method": "ctrl+space",
    "rules": {
        "weixin.exe": "chinese",
        "powerpnt.exe": "chinese",
        "winword.exe": "chinese",
        "wpp.exe": "chinease",
        "wps.exe": "chinease"
    }
}


```
