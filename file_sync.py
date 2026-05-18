#!/usr/bin/env python3
"""
文件自动同步守护进程 (实验项目版)
监听本项目内部的投放区文件夹，自动将文件同步到 WebServer root 目录并生成 index.html
"""
import os, sys, time, shutil, signal
from pathlib import Path
from datetime import datetime
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler

# ── 终端颜色配置 ──
class Color:
    RESET = '\033[0m'
    BOLD = '\033[1m'
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'

# ── 路径配置 (锁定在项目内部) ──
# 获取当前脚本所在绝对路径（假设脚本在项目根目录）
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

WATCH_DIR    = os.path.join(BASE_DIR, "dropzone")  # 投放区锁定在当前项目下
WEB_ROOT     = os.path.join(BASE_DIR, "build", "Debug", "root")  # 目标目录
INDEX_FILE   = os.path.join(WEB_ROOT, "index.html")

IGNORE_FILES = {"index.html", "judge.html", "welcome.html", "log.html", 
                "logError.html", "register.html", "registerError.html",
                "picture.html", "video.html", "fans.html", "favicon.ico",
                "dashboard.html", "mysql.html", "threadpool.html",
                "timer.html", "logger.html", "config.html"}

def ensure_dirs():
    os.makedirs(WATCH_DIR, exist_ok=True)
    os.makedirs(WEB_ROOT, exist_ok=True)

def generate_index():
    """扫描 WEB_ROOT 下所有文件，生成一个纯净版的 index.html"""
    files = []
    for entry in sorted(Path(WEB_ROOT).iterdir(), key=lambda p: p.stat().st_mtime, reverse=True):
        if entry.is_dir(): continue
        if entry.name in IGNORE_FILES: continue
        
        size_kb = entry.stat().st_size / 1024
        mtime   = datetime.fromtimestamp(entry.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
        ext     = entry.suffix.lower()
        
        # 将图形图标替换为纯文本的后缀名标签
        ext_label = ext[1:].upper() if len(ext) > 1 else "FILE"
        files.append((entry.name, size_kb, mtime, ext_label))

    html = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0"><title>Local File Server</title>
<style>
:root{{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#c9d1d9;--muted:#8b949e;--blue:#58a6ff;--label:#238636}}
*{{margin:0;padding:0;box-sizing:border-box}}
body{{background:var(--bg);color:var(--text);font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono","Courier New",monospace;min-height:100vh}}
.header{{display:flex;align-items:center;justify-content:space-between;padding:18px 28px;border-bottom:1px solid var(--border);background:var(--card)}}
.header h1{{font-size:1.2rem; font-weight:normal;}}
.header .count{{color:var(--muted);font-size:0.85rem}}
.file-grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:14px;padding:24px 28px}}
.file-card{{background:var(--card);border:1px solid var(--border);border-radius:6px;padding:16px;transition:border-color .15s;text-align:center;text-decoration:none;color:var(--text)}}
.file-card:hover{{border-color:var(--blue);}}
.file-card .icon{{font-size:0.8rem;color:var(--label);margin-bottom:10px;border:1px solid var(--border);border-radius:4px;display:inline-block;padding:2px 8px}}
.file-card .name{{font-size:0.85rem;word-break:break-all;margin-bottom:8px;line-height:1.4}}
.file-card .meta{{color:var(--muted);font-size:0.75rem}}
.footer{{text-align:center;padding:20px;color:var(--muted);font-size:0.75rem;border-top:1px solid var(--border);position:fixed;bottom:0;width:100%;background:var(--bg)}}
</style></head><body>
<div class="header">
    <h1>[ Local File Server ]</h1>
    <span class="count">Total: {len(files)} files | Updated: {datetime.now().strftime('%H:%M:%S')}</span>
</div>
<div class="file-grid">
"""
    for name, size, mtime, ext_label in files:
        # 从文件名提取后缀，而非依赖上一个循环的残留变量
        cur_ext = os.path.splitext(name)[1].lower()

        # 如果是视频文件 (mp4, webm)，直接渲染 HTML5 视频播放器
        if cur_ext in ['.mp4', '.webm']:
            html += f'''
            <div class="file-card" style="cursor: default;">
                <div class="icon">[{ext_label}]</div>
                <div class="name">{name}</div>
                <video width="100%" controls preload="metadata" style="border-radius: 4px; margin-top: 8px; background: #000;">
                    <source src="/{name}" type="video/mp4">
                    您的浏览器不支持视频播放。
                </video>
                <div class="meta" style="margin-top: 10px;">{size:.1f} KB | {mtime}</div>
            </div>\n'''
        # 如果是图片，也可以直接显示缩略图
        elif cur_ext in ['.jpg', '.jpeg', '.png', '.gif']:
            html += f'''
            <a class="file-card" href="/{name}" target="_blank" title="{name} ({size:.1f} KB)">
                <div class="icon">[{ext_label}]</div>
                <div class="name">{name}</div>
                <img src="/{name}" style="width: 100%; height: 120px; object-fit: cover; border-radius: 4px; margin-top: 8px;" loading="lazy">
                <div class="meta" style="margin-top: 10px;">{size:.1f} KB | {mtime}</div>
            </a>\n'''
        # 其他文件（代码、压缩包等），保持原本的点击下载/查看逻辑
        else:
            html += f'''
            <a class="file-card" href="/{name}" target="_blank" title="{name} ({size:.1f} KB)">
                <div class="icon">[{ext_label}]</div>
                <div class="name">{name}</div>
                <div class="meta" style="margin-top: 20px;">{size:.1f} KB | {mtime}</div>
            </a>\n'''

    with open(INDEX_FILE, "w", encoding="utf-8") as f:
        f.write(html)
    
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {Color.CYAN}[INDEX]{Color.RESET} {INDEX_FILE} generated ({len(files)} files)")


class DropzoneHandler(FileSystemEventHandler):
    def on_created(self, event):
        if event.is_directory: return
        src = event.src_path
        fname = os.path.basename(src)
        if fname.startswith("."): return
        
        time.sleep(0.5)
        dst = os.path.join(WEB_ROOT, fname)
        try:
            shutil.copy2(src, dst)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] {Color.GREEN}[SYNC]{Color.RESET} {fname}")
            generate_index()
        except Exception as e:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] {Color.RED}[ERROR]{Color.RESET} Failed to copy {fname}: {e}")

    def on_deleted(self, event):
        if event.is_directory: return
        fname = os.path.basename(event.src_path)
        dst = os.path.join(WEB_ROOT, fname)
        if os.path.exists(dst):
            os.remove(dst)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] {Color.YELLOW}[DELETE]{Color.RESET} {fname}")
            generate_index()

    def on_modified(self, event):
        if event.is_directory: return
        fname = os.path.basename(event.src_path)
        if fname.startswith("."): return
        time.sleep(0.5)
        dst = os.path.join(WEB_ROOT, fname)
        try:
            shutil.copy2(event.src_path, dst)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] {Color.BLUE}[UPDATE]{Color.RESET} {fname}")
            generate_index()
        except: pass


def main():
    print(f"{Color.BOLD}{'=' * 55}{Color.RESET}")
    print(f" {Color.BOLD}[ WebServer Sync Daemon ]{Color.RESET}")
    print(f" {Color.CYAN}Watch Dir:{Color.RESET} {WATCH_DIR}")
    print(f" {Color.CYAN}Target Dir:{Color.RESET} {WEB_ROOT}")
    print(f" {Color.YELLOW}Press Ctrl+C to stop{Color.RESET}")
    print(f"{Color.BOLD}{'=' * 55}{Color.RESET}")

    ensure_dirs()
    generate_index()

    signal.signal(signal.SIGINT, lambda s, f: sys.exit(0))

    observer = Observer()
    observer.schedule(DropzoneHandler(), WATCH_DIR, recursive=False)
    observer.start()
    
    print(f"\n{Color.BOLD}Listening for events in {WATCH_DIR}...{Color.RESET}\n")

    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        observer.stop()
    observer.join()
    print(f"\n{Color.YELLOW}[STOPPED]{Color.RESET} Sync daemon terminated.")

if __name__ == "__main__":
    main()