#!/usr/bin/env python3
"""
Remove comments from files in a repository. Handles C/C++-style // and /* */, PowerShell <# #> and #, shell-style #, AutoHotkey ;.
It will remove comment-only lines and inline comments, and avoid leaving blank lines where whole comment lines were removed.

Usage:
    python tools/remove_comments.py [file ...]

If no files are given, it defaults to include/, src/, resources/, tools/, and CMakeLists.txt.

The script prints the list of files processed and exits with non-zero if errors.
"""

import sys
import os
import re
from pathlib import Path

# Define file types and comment rules
C_STYLE_EXTS = {'.c', '.cpp', '.cc', '.cxx', '.h', '.hpp', '.hh', '.rc', '.qrc', '.mm'}
HASH_STYLE_EXTS = {'.ps1', '.sh', '.bash', '.zsh', '.fish', '.cmake'}
SEMI_STYLE_EXTS = {'.ahk'}

# PowerShell also supports block comments <# #> - treat as C-style block comments but with different markers


class CommentRemover:
    def __init__(self, text, style, powershell_block=False):
        self.text = text
        self.style = style
        self.power_block = powershell_block

    def _is_ident(self, ch):
        return ch.isalnum() or ch == '_'

    def remove_c_style(self):
        s = self.text
        n = len(s)
        out_chars = []
        i = 0
        in_string = False
        in_char = False
        string_delim = ''
        escape = False
        in_line_comment = False
        in_block_comment = False

        while i < n:
            ch = s[i]
            nxt = s[i+1] if i+1 < n else ''

            if in_line_comment:
                if ch == '\n':
                    in_line_comment = False
                    out_chars.append(ch)
                i += 1
                continue

            if in_block_comment:
                if ch == '*' and nxt == '/':
                    in_block_comment = False
                    i += 2
                else:
                    i += 1
                continue

            if escape:
                out_chars.append(ch)
                escape = False
                i += 1
                continue

            if ch == '\\' and (in_string or in_char):
                escape = True
                out_chars.append(ch)
                i += 1
                continue

            if in_string or in_char:
                out_chars.append(ch)
                if ch == string_delim:
                    in_string = False
                    in_char = False
                i += 1
                continue

            # Not in string or comment
            # Detect string or char start
            if ch == '"' or ch == '\'':
                in_string = True
                string_delim = ch
                out_chars.append(ch)
                i += 1
                continue

            if ch == '/' and nxt == '/':
                # start line comment
                in_line_comment = True
                i += 2
                # If preceding char and next non-comment char are identifier, insert a space to avoid token joining
                continue

            if ch == '/' and nxt == '*':
                in_block_comment = True
                i += 2
                continue

            out_chars.append(ch)
            i += 1

        return ''.join(out_chars)

    def remove_hash_style(self, powershell_block=False):
        s = self.text
        n = len(s)
        out_chars = []
        i = 0
        in_string = False
        string_delim = ''
        escape = False
        in_line_comment = False
        in_block_comment = False

        while i < n:
            ch = s[i]
            nxt = s[i+1] if i+1 < n else ''

            if in_line_comment:
                if ch == '\n':
                    in_line_comment = False
                    out_chars.append(ch)
                i += 1
                continue

            if in_block_comment:
                # detect PowerShell <# #>
                if ch == '#' and nxt == '>':
                    in_block_comment = False
                    i += 2
                else:
                    i += 1
                continue

            if escape:
                out_chars.append(ch)
                escape = False
                i += 1
                continue

            if ch == '\\' and in_string:
                escape = True
                out_chars.append(ch)
                i += 1
                continue

            if in_string:
                out_chars.append(ch)
                if ch == string_delim:
                    in_string = False
                i += 1
                continue

            if ch == '"' or ch == '\'':
                in_string = True
                string_delim = ch
                out_chars.append(ch)
                i += 1
                continue

            # detect line comment '#'
            if ch == '#' and not (nxt == '>' and i>0 and s[i-1] == '<'):
                # start of line comment or inline
                in_line_comment = True
                i += 1
                continue

            if powershell_block and ch == '<' and nxt == '#':
                in_block_comment = True
                i += 2
                continue

            out_chars.append(ch)
            i += 1

        return ''.join(out_chars)

    def remove_semi_style(self):
        # AHK uses ';' for line comments
        s = self.text
        n = len(s)
        out_chars = []
        i = 0
        in_string = False
        string_delim = ''
        escape = False
        in_line_comment = False

        while i < n:
            ch = s[i]
            nxt = s[i+1] if i+1 < n else ''

            if in_line_comment:
                if ch == '\n':
                    in_line_comment = False
                    out_chars.append(ch)
                i += 1
                continue

            if escape:
                out_chars.append(ch)
                escape = False
                i += 1
                continue

            if ch == '\\' and in_string:
                escape = True
                out_chars.append(ch)
                i += 1
                continue

            if in_string:
                out_chars.append(ch)
                if ch == string_delim:
                    in_string = False
                i += 1
                continue

            if ch == '"' or ch == '\'':
                in_string = True
                string_delim = ch
                out_chars.append(ch)
                i += 1
                continue

            if ch == ';':
                in_line_comment = True
                i += 1
                continue

            out_chars.append(ch)
            i += 1

        return ''.join(out_chars)


def process_file(path: Path):
    ext = path.suffix.lower()
    try:
        text = path.read_text(encoding='utf-8')
    except Exception:
        text = path.read_text(encoding='latin-1')

    if ext in C_STYLE_EXTS:
        cleaner = CommentRemover(text, 'c')
        new = cleaner.remove_c_style()
    elif ext in HASH_STYLE_EXTS or path.name == 'CMakeLists.txt' or path.suffix.lower() == '.cmake':
        # detect PowerShell specifically
        if ext == '.ps1':
            cleaner = CommentRemover(text, 'hash', powershell_block=True)
            new = cleaner.remove_hash_style(True)
        else:
            cleaner = CommentRemover(text, 'hash')
            new = cleaner.remove_hash_style(False)
    elif ext in SEMI_STYLE_EXTS:
        cleaner = CommentRemover(text, 'semi')
        new = cleaner.remove_semi_style()
    else:
        # For unknown types, don't change
        return False, "skipped"

    # Post-process to remove lines that were fully comments (be careful to keep original blank lines)
    old_lines = text.splitlines()
    new_lines = new.splitlines()

    final_lines = []
    # Align original and new lines by iterating through old_lines and mapping to new_lines using pointer
    # If block comments removed multiple lines, mapping is complex. We'll take a simpler approach:
    # If a new line is empty (after strip), and original line had a comment marker (//, /*, #, ;), consider it removed and skip it.

    comment_markers = ['//', '/*', '#', ';', '<#']

    j = 0
    for i, old in enumerate(old_lines):
        new_line = new_lines[j] if j < len(new_lines) else ''
        old_strip = old.strip()
        new_strip = new_line.strip()

        has_comment_marker = any(old_strip.startswith(m) for m in comment_markers) or '/*' in old or '*/' in old

        if new_strip == '' and has_comment_marker:
            # skip this line
            pass
        else:
            final_lines.append(new_line)
        j += 1

    # If new produce fewer lines leftover mapping fails, fall back to simple blanking: keep non-empty lines
    if j < len(new_lines):
        final_lines = [ln for ln in new_lines if ln.strip() != '']

    final_text = '\n'.join(final_lines)
    if text.endswith('\n'):
        final_text += '\n'

    changed = final_text != text
    if changed:
        path.write_text(final_text, encoding='utf-8')
    return changed, 'changed' if changed else 'unchanged'


# Main routine
if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument('files', nargs='*', help='Files or directories to process')
    args = parser.parse_args()

    roots = args.files if args.files else ['include', 'src', 'resources', 'tools', 'CMakeLists.txt', 'convert_icon.ps1']

    # Expand files
    paths = []
    for r in roots:
        p = Path(r)
        if p.is_dir():
            for fp in p.rglob('*'):
                if fp.is_file():
                    paths.append(fp)
        elif p.is_file():
            paths.append(p)

    # Filter to known extensions
    candidate_exts = set(C_STYLE_EXTS) | set(HASH_STYLE_EXTS) | set(SEMI_STYLE_EXTS) | {'.cmake', '.txt', '.ps1'}

    changes = []
    for fp in sorted(paths):
        if fp.suffix.lower() in candidate_exts or fp.name == 'CMakeLists.txt' or fp.suffix == '.ps1' or fp.suffix == '.ahk' or fp.suffix == '.ps1':
            changed, status = process_file(fp)
            if changed:
                changes.append(str(fp))

    print('Processed', len(paths), 'files; changed', len(changes), 'files')
    for c in changes:
        print(' -', c)

    if changes:
        sys.exit(0)
    else:
        sys.exit(0)
